// Copyright 2020 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "redpanda/application.h"

#include "archival/ntp_archiver_service.h"
#include "archival/service.h"
#include "cluster/cluster_utils.h"
#include "cluster/id_allocator.h"
#include "cluster/id_allocator_frontend.h"
#include "cluster/metadata_dissemination_handler.h"
#include "cluster/metadata_dissemination_service.h"
#include "cluster/partition_manager.h"
#include "cluster/security_frontend.h"
#include "cluster/service.h"
#include "cluster/topics_frontend.h"
#include "config/configuration.h"
#include "config/endpoint_tls_config.h"
#include "config/seed_server.h"
#include "kafka/client/configuration.h"
#include "kafka/server/coordinator_ntp_mapper.h"
#include "kafka/server/group_manager.h"
#include "kafka/server/group_router.h"
#include "kafka/server/protocol.h"
#include "kafka/server/quota_manager.h"
#include "model/metadata.h"
#include "pandaproxy/configuration.h"
#include "pandaproxy/proxy.h"
#include "platform/stop_signal.h"
#include "raft/service.h"
#include "redpanda/admin/api-doc/config.json.h"
#include "redpanda/admin/api-doc/kafka.json.h"
#include "redpanda/admin/api-doc/partition.json.h"
#include "redpanda/admin/api-doc/raft.json.h"
#include "redpanda/admin/api-doc/security.json.h"
#include "resource_mgmt/io_priority.h"
#include "rpc/simple_protocol.h"
#include "security/scram_algorithm.h"
#include "security/scram_authenticator.h"
#include "storage/chunk_cache.h"
#include "storage/directories.h"
#include "syschecks/syschecks.h"
#include "test_utils/logs.h"
#include "utils/file_io.h"
#include "version.h"
#include "vlog.h"

#include <seastar/core/metrics.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/http/api_docs.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/file_handler.hh>
#include <seastar/json/json_elements.hh>
#include <seastar/net/tls.hh>
#include <seastar/util/defer.hh>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sys/utsname.h>

#include <chrono>
#include <exception>
#include <vector>

application::application(ss::sstring logger_name)
  : _log(std::move(logger_name)){

  };

int application::run(int ac, char** av) {
    init_env();
    vlog(_log.info, "Redpanda {}", redpanda_version());
    struct ::utsname buf;
    ::uname(&buf);
    vlog(
      _log.info,
      "kernel={}, nodename={}, machine={}",
      buf.release,
      buf.nodename,
      buf.machine);
    ss::app_template app = setup_app_template();
    return app.run(ac, av, [this, &app] {
        auto& cfg = app.configuration();
        validate_arguments(cfg);
        return ss::async([this, &cfg] {
            try {
                ::stop_signal app_signal;
                auto deferred = ss::defer([this] {
                    auto deferred = std::move(_deferred);
                    // stop services in reverse order
                    while (!deferred.empty()) {
                        deferred.pop_back();
                    }
                });
                // must initialize configuration before services
                hydrate_config(cfg);
                initialize();
                check_environment();
                setup_metrics();
                configure_admin_server();
                wire_up_services();
                start();
                app_signal.wait().get();
                vlog(_log.info, "Stopping...");
            } catch (...) {
                vlog(
                  _log.info,
                  "Failure during startup: {}",
                  std::current_exception());
                return 1;
            }
            return 0;
        });
    });
}

void application::initialize(
  std::optional<YAML::Node> proxy_cfg,
  std::optional<YAML::Node> proxy_client_cfg,
  std::optional<scheduling_groups> groups) {
    if (config::shard_local_cfg().enable_pid_file()) {
        syschecks::pidfile_create(config::shard_local_cfg().pidfile_path());
    }

    smp_service_groups.create_groups().get();
    _deferred.emplace_back(
      [this] { smp_service_groups.destroy_groups().get(); });

    if (groups) {
        _scheduling_groups = *groups;
        return;
    }

    _scheduling_groups.create_groups().get();
    _deferred.emplace_back(
      [this] { _scheduling_groups.destroy_groups().get(); });

    if (proxy_cfg) {
        _proxy_config.emplace(*proxy_cfg);
    }

    if (proxy_client_cfg) {
        _proxy_client_config.emplace(*proxy_client_cfg);
    }
}

void application::setup_metrics() {
    if (!config::shard_local_cfg().disable_metrics()) {
        _metrics.add_group(
          "application",
          {ss::metrics::make_gauge(
            "uptime",
            [] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                         ss::engine().uptime())
                  .count();
            },
            ss::metrics::description("Redpanda uptime in milliseconds"))});
    }
}

void application::validate_arguments(const po::variables_map& cfg) {
    if (!cfg.count("redpanda-cfg")) {
        throw std::invalid_argument("Missing redpanda-cfg flag");
    }
}

void application::init_env() { std::setvbuf(stdout, nullptr, _IOLBF, 1024); }

ss::app_template application::setup_app_template() {
    ss::app_template::config app_cfg;
    app_cfg.name = "Redpanda";
    using namespace std::literals::chrono_literals; // NOLINT
    app_cfg.default_task_quota = 500us;
    app_cfg.auto_handle_sigint_sigterm = false;
    auto app = ss::app_template(app_cfg);
    app.add_options()(
      "redpanda-cfg",
      po::value<std::string>(),
      ".yaml file config for redpanda");
    return app;
}

void application::hydrate_config(const po::variables_map& cfg) {
    std::filesystem::path cfg_path(cfg["redpanda-cfg"].as<std::string>());
    auto buf = read_fully(cfg_path).get0();
    // see https://github.com/jbeder/yaml-cpp/issues/765
    auto workaround = ss::uninitialized_string(buf.size_bytes());
    auto in = iobuf::iterator_consumer(buf.cbegin(), buf.cend());
    in.consume_to(buf.size_bytes(), workaround.begin());
    const YAML::Node config = YAML::Load(workaround);
    vlog(_log.info, "Configuration:\n\n{}\n\n", config);
    vlog(
      _log.info,
      "Use `rpk config set <cfg> <value>` to change values "
      "below:");
    auto config_printer = [this](std::string_view service) {
        return [this, service](const config::base_property& item) {
            std::stringstream val;
            item.print(val);
            vlog(_log.info, "{}.{}\t- {}", service, val.str(), item.desc());
        };
    };
    _redpanda_enabled = config["redpanda"];
    if (_redpanda_enabled) {
        ss::smp::invoke_on_all([&config] {
            config::shard_local_cfg().read_yaml(config);
        }).get0();
        config::shard_local_cfg().for_each(config_printer("redpanda"));
    }
    if (config["pandaproxy"]) {
        _proxy_config.emplace(config["pandaproxy"]);
        if (config["pandaproxy_client"]) {
            _proxy_client_config.emplace(config["pandaproxy_client"]);
        } else {
            _proxy_client_config.emplace();
            const auto& kafka_api = config::shard_local_cfg().kafka_api.value();
            vassert(!kafka_api.empty(), "There are no kafka_api listeners");
            _proxy_client_config->brokers.set_value(
              std::vector<unresolved_address>{kafka_api[0].address});
        }
        _proxy_config->for_each(config_printer("pandaproxy"));
        _proxy_client_config->for_each(config_printer("pandaproxy_client"));
    }
}

void application::check_environment() {
    syschecks::systemd_message("checking environment (CPU, Mem)").get();
    syschecks::cpu();
    syschecks::memory(config::shard_local_cfg().developer_mode());
    if (_redpanda_enabled) {
        storage::directories::initialize(
          config::shard_local_cfg().data_directory().as_sstring())
          .get();
    }
}

/**
 * Prepend a / to the path component. This handles the case where path is an
 * empty string (e.g. url/) or when the path omits the root file path directory
 * (e.g. url/index.html vs url//index.html). The directory handler in seastar is
 * opininated and not very forgiving here so we help it a bit.
 */
class dashboard_handler final : public ss::httpd::directory_handler {
public:
    dashboard_handler()
      : directory_handler(*config::shard_local_cfg().dashboard_dir()) {}

    ss::future<std::unique_ptr<ss::httpd::reply>> handle(
      const ss::sstring& path,
      std::unique_ptr<ss::httpd::request> req,
      std::unique_ptr<ss::httpd::reply> rep) override {
        req->param.set("path", "/" + req->param.at("path"));
        return directory_handler::handle(path, std::move(req), std::move(rep));
    }
};

void application::configure_admin_server() {
    auto& conf = config::shard_local_cfg();
    if (!conf.enable_admin_api()) {
        return;
    }
    syschecks::systemd_message("constructing http server").get();
    construct_service(_admin, ss::sstring("admin")).get();
    // configure admin API TLS
    if (conf.admin_api_tls().is_enabled()) {
        _admin
          .invoke_on_all([this](ss::http_server& server) {
              return config::shard_local_cfg()
                .admin_api_tls()
                .get_credentials_builder()
                .then([this, &server](
                        std::optional<ss::tls::credentials_builder> builder) {
                    if (!builder) {
                        return ss::now();
                    }

                    return builder
                      ->build_reloadable_server_credentials(
                        [this](
                          const std::unordered_set<ss::sstring>& updated,
                          const std::exception_ptr& eptr) {
                            cluster::log_certificate_reload_event(
                              _log, "API TLS", updated, eptr);
                        })
                      .then([&server](auto cred) {
                          server.set_tls_credentials(std::move(cred));
                      });
                });
          })
          .get0();
    }
    if (conf.dashboard_dir()) {
        _admin
          .invoke_on_all([](ss::http_server& server) {
              server._routes.add(
                ss::httpd::operation_type::GET,
                ss::httpd::url("/dashboard").remainder("path"),
                new dashboard_handler());
          })
          .get0();
    }
    ss::prometheus::config metrics_conf;
    metrics_conf.metric_help = "redpanda metrics";
    metrics_conf.prefix = "vectorized";
    ss::prometheus::add_prometheus_routes(_admin, metrics_conf).get();
    if (conf.enable_admin_api()) {
        syschecks::systemd_message(
          "enabling admin HTTP api: {}", config::shard_local_cfg().admin())
          .get();
        auto rb = ss::make_shared<ss::api_registry_builder20>(
          conf.admin_api_doc_dir(), "/v1");
        _admin
          .invoke_on_all([this, rb](ss::http_server& server) {
              auto insert_comma = [](ss::output_stream<char>& os) {
                  return os.write(",\n");
              };
              rb->set_api_doc(server._routes);
              rb->register_api_file(server._routes, "header");
              rb->register_api_file(server._routes, "config");
              rb->register_function(server._routes, insert_comma);
              rb->register_api_file(server._routes, "raft");
              rb->register_function(server._routes, insert_comma);
              rb->register_api_file(server._routes, "kafka");
              rb->register_function(server._routes, insert_comma);
              rb->register_api_file(server._routes, "partition");
              rb->register_function(server._routes, insert_comma);
              rb->register_api_file(server._routes, "security");
              ss::httpd::config_json::get_config.set(
                server._routes, []([[maybe_unused]] ss::const_req req) {
                    rapidjson::StringBuffer buf;
                    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
                    config::shard_local_cfg().to_json(writer);
                    return ss::json::json_return_type(buf.GetString());
                });
              admin_register_raft_routes(server);
              admin_register_kafka_routes(server);
              admin_register_security_routes(server);
          })
          .get();
    }

    with_scheduling_group(_scheduling_groups.admin_sg(), [this] {
        return rpc::resolve_dns(config::shard_local_cfg().admin())
          .then([this](ss::socket_address addr) mutable {
              return _admin
                .invoke_on_all<ss::future<> (ss::http_server::*)(
                  ss::socket_address)>(&ss::http_server::listen, addr)
                .handle_exception([this](auto ep) {
                    _log.error("Exception on http admin server: {}", ep);
                    return ss::make_exception_future<>(ep);
                });
          });
    }).get();

    vlog(_log.info, "Started HTTP admin service listening at {}", conf.admin());
}

static storage::kvstore_config kvstore_config_from_global_config() {
    /*
     * The key-value store is rooted at the configured data directory, and
     * the internal kvstore topic-namespace results in a storage layout of:
     *
     *    /var/lib/redpanda/data/
     *       - redpanda/kvstore/
     *           - 0
     *           - 1
     *           - ... #cores
     */
    return storage::kvstore_config(
      config::shard_local_cfg().kvstore_max_segment_size(),
      config::shard_local_cfg().kvstore_flush_interval(),
      config::shard_local_cfg().data_directory().as_sstring(),
      storage::debug_sanitize_files::no);
}

static storage::log_config manager_config_from_global_config() {
    return storage::log_config(
      storage::log_config::storage_type::disk,
      config::shard_local_cfg().data_directory().as_sstring(),
      config::shard_local_cfg().log_segment_size(),
      config::shard_local_cfg().compacted_log_segment_size(),
      config::shard_local_cfg().max_compacted_log_segment_size(),
      storage::debug_sanitize_files::no,
      priority_manager::local().compaction_priority(),
      config::shard_local_cfg().retention_bytes(),
      config::shard_local_cfg().log_compaction_interval_ms(),
      config::shard_local_cfg().delete_retention_ms(),
      storage::with_cache(!config::shard_local_cfg().disable_batch_cache()),
      storage::batch_cache::reclaim_options{
        .growth_window = config::shard_local_cfg().reclaim_growth_window(),
        .stable_window = config::shard_local_cfg().reclaim_stable_window(),
        .min_size = config::shard_local_cfg().reclaim_min_size(),
        .max_size = config::shard_local_cfg().reclaim_max_size(),
      });
}

// add additional services in here
void application::wire_up_services() {
    if (_redpanda_enabled) {
        wire_up_redpanda_services();
    }
    if (_proxy_config) {
        construct_service(
          _proxy, to_yaml(*_proxy_config), to_yaml(*_proxy_client_config))
          .get();
    }
}

void application::wire_up_redpanda_services() {
    ss::smp::invoke_on_all([] {
        return storage::internal::chunks().start();
    }).get();

    // cluster
    syschecks::systemd_message("Adding raft client cache").get();
    construct_service(_raft_connection_cache).get();
    syschecks::systemd_message("Building shard-lookup tables").get();
    construct_service(shard_table).get();

    syschecks::systemd_message("Intializing storage services").get();
    auto log_cfg = manager_config_from_global_config();
    log_cfg.reclaim_opts.background_reclaimer_sg
      = _scheduling_groups.cache_background_reclaim_sg();
    construct_service(storage, kvstore_config_from_global_config(), log_cfg)
      .get();

    if (coproc_enabled()) {
        auto coproc_supervisor_server_addr
          = rpc::resolve_dns(
              config::shard_local_cfg().coproc_supervisor_server())
              .get0();
        syschecks::systemd_message("Building coproc pacemaker").get();
        construct_service(
          pacemaker, coproc_supervisor_server_addr, std::ref(storage))
          .get();
    }

    syschecks::systemd_message("Intializing raft group manager").get();
    construct_service(
      raft_group_manager,
      model::node_id(config::shard_local_cfg().node_id()),
      config::shard_local_cfg().raft_io_timeout_ms(),
      config::shard_local_cfg().raft_heartbeat_interval_ms(),
      config::shard_local_cfg().raft_heartbeat_timeout_ms(),
      std::ref(_raft_connection_cache),
      std::ref(storage))
      .get();

    syschecks::systemd_message("Adding partition manager").get();
    construct_service(
      partition_manager, std::ref(storage), std::ref(raft_group_manager))
      .get();
    vlog(_log.info, "Partition manager started");

    // controller

    syschecks::systemd_message("Creating cluster::controller").get();

    construct_single_service(
      controller,
      _raft_connection_cache,
      partition_manager,
      shard_table,
      storage);

    controller->wire_up().get0();
    syschecks::systemd_message("Creating kafka metadata cache").get();
    construct_service(
      metadata_cache,
      std::ref(controller->get_topics_state()),
      std::ref(controller->get_members_table()),
      std::ref(controller->get_partition_leaders()))
      .get();

    syschecks::systemd_message("Creating metadata dissemination service").get();
    construct_service(
      md_dissemination_service,
      std::ref(raft_group_manager),
      std::ref(partition_manager),
      std::ref(controller->get_partition_leaders()),
      std::ref(controller->get_members_table()),
      std::ref(controller->get_topics_state()),
      std::ref(_raft_connection_cache))
      .get();

    if (archival_storage_enabled()) {
        syschecks::systemd_message("Starting archival scheduler").get();
        ss::sharded<archival::configuration> configs;
        configs.start().get();
        configs
          .invoke_on_all([](archival::configuration& c) {
              return archival::scheduler_service::get_archival_service_config()
                .then(
                  [&c](archival::configuration cfg) { c = std::move(cfg); });
          })
          .get();
        construct_service(
          archival_scheduler,
          std::ref(storage),
          std::ref(partition_manager),
          std::ref(controller->get_topics_state()),
          std::ref(configs))
          .get();
        configs.stop().get();
    }
    // group membership
    syschecks::systemd_message("Creating partition manager").get();
    construct_service(
      _group_manager,
      std::ref(raft_group_manager),
      std::ref(partition_manager),
      std::ref(controller->get_topics_state()),
      std::ref(config::shard_local_cfg()))
      .get();
    syschecks::systemd_message("Creating kafka group shard mapper").get();
    construct_service(coordinator_ntp_mapper, std::ref(metadata_cache)).get();
    syschecks::systemd_message("Creating kafka group router").get();
    construct_service(
      group_router,
      _scheduling_groups.kafka_sg(),
      smp_service_groups.kafka_smp_sg(),
      std::ref(_group_manager),
      std::ref(shard_table),
      std::ref(coordinator_ntp_mapper))
      .get();

    // metrics and quota management
    syschecks::systemd_message("Adding kafka quota manager").get();
    construct_service(quota_mgr).get();
    // rpc
    ss::sharded<rpc::server_configuration> rpc_cfg;
    rpc_cfg.start(ss::sstring("internal_rpc")).get();
    rpc_cfg
      .invoke_on_all([this](rpc::server_configuration& c) {
          return ss::async([this, &c] {
              auto rpc_server_addr = rpc::resolve_dns(
                                       config::shard_local_cfg().rpc_server())
                                       .get0();
              c.load_balancing_algo
                = ss::server_socket::load_balancing_algorithm::port;
              c.max_service_memory_per_core = memory_groups::rpc_total_memory();
              c.disable_metrics = rpc::metrics_disabled(
                config::shard_local_cfg().disable_metrics());
              auto rpc_builder = config::shard_local_cfg()
                                   .rpc_server_tls()
                                   .get_credentials_builder()
                                   .get0();
              auto credentials
                = rpc_builder
                    ? rpc_builder
                        ->build_reloadable_server_credentials(
                          [this](
                            const std::unordered_set<ss::sstring>& updated,
                            const std::exception_ptr& eptr) {
                              cluster::log_certificate_reload_event(
                                _log, "Internal RPC TLS", updated, eptr);
                          })
                        .get0()
                    : nullptr;
              c.addrs.emplace_back(rpc_server_addr, credentials);
          });
      })
      .get();
    /**
     * Use port based load_balancing_algorithm to make connection shard
     * assignment deterministic.
     **/
    syschecks::systemd_message("Starting internal RPC {}", rpc_cfg.local())
      .get();
    construct_service(_rpc, &rpc_cfg).get();
    rpc_cfg.stop().get();

    syschecks::systemd_message("Creating id allocator frontend").get();
    construct_service(
      id_allocator_frontend,
      smp_service_groups.raft_smp_sg(),
      std::ref(partition_manager),
      std::ref(shard_table),
      std::ref(metadata_cache),
      std::ref(_raft_connection_cache),
      std::ref(controller->get_partition_leaders()),
      std::ref(controller))
      .get();

    ss::sharded<rpc::server_configuration> kafka_cfg;
    kafka_cfg.start(ss::sstring("kafka_rpc")).get();
    kafka_cfg
      .invoke_on_all([this](rpc::server_configuration& c) {
          return ss::async([this, &c] {
              c.max_service_memory_per_core
                = memory_groups::kafka_total_memory();
              auto& tls_config
                = config::shard_local_cfg().kafka_api_tls.value();
              for (const auto& ep : config::shard_local_cfg().kafka_api()) {
                  ss::shared_ptr<ss::tls::server_credentials> credentails;
                  // find credentials for this endpoint
                  auto it = find_if(
                    tls_config.begin(),
                    tls_config.end(),
                    [&ep](const config::endpoint_tls_config& cfg) {
                        return cfg.name == ep.name;
                    });
                  // if tls is configured for this endpoint build reloadable
                  // credentails
                  if (it != tls_config.end()) {
                      syschecks::systemd_message(
                        "Building TLS credentials for kafka")
                        .get();
                      auto kafka_builder
                        = it->config.get_credentials_builder().get0();
                      credentails
                        = kafka_builder
                            ? kafka_builder
                                ->build_reloadable_server_credentials(
                                  [this, name = it->name](
                                    const std::unordered_set<ss::sstring>&
                                      updated,
                                    const std::exception_ptr& eptr) {
                                      cluster::log_certificate_reload_event(
                                        _log, "Kafka RPC TLS", updated, eptr);
                                  })
                                .get0()
                            : nullptr;
                  }

                  c.addrs.emplace_back(
                    ep.name, rpc::resolve_dns(ep.address).get0(), credentails);
              }

              c.disable_metrics = rpc::metrics_disabled(
                config::shard_local_cfg().disable_metrics());
          });
      })
      .get();
    syschecks::systemd_message("Starting kafka RPC {}", kafka_cfg.local())
      .get();
    construct_service(_kafka_server, &kafka_cfg).get();
    kafka_cfg.stop().get();
    construct_service(
      fetch_session_cache,
      config::shard_local_cfg().fetch_session_eviction_timeout_ms())
      .get();
}

ss::future<> application::set_proxy_config(ss::sstring name, std::any val) {
    return _proxy.invoke_on_all(
      [name{std::move(name)}, val{std::move(val)}](pandaproxy::proxy& p) {
          p.config().get(name).set_value(val);
      });
}

bool application::archival_storage_enabled() {
    const auto& cfg = config::shard_local_cfg();
    return cfg.cloud_storage_enabled();
}

ss::future<>
application::set_proxy_client_config(ss::sstring name, std::any val) {
    return _proxy.invoke_on_all(
      [name{std::move(name)}, val{std::move(val)}](pandaproxy::proxy& p) {
          p.client_config().get(name).set_value(val);
      });
}

void application::start() {
    if (_redpanda_enabled) {
        start_redpanda();
    }

    if (_proxy_config) {
        _proxy.invoke_on_all(&pandaproxy::proxy::start).get();
        vlog(
          _log.info,
          "Started Pandaproxy listening at {}",
          _proxy_config->pandaproxy_api());
    }

    vlog(_log.info, "Successfully started Redpanda!");
    syschecks::systemd_notify_ready().get();
}

void application::start_redpanda() {
    syschecks::systemd_message("Staring storage services").get();
    storage.invoke_on_all(&storage::api::start).get();

    syschecks::systemd_message("Starting the partition manager").get();
    partition_manager.invoke_on_all(&cluster::partition_manager::start).get();

    syschecks::systemd_message("Starting Raft group manager").get();
    raft_group_manager.invoke_on_all(&raft::group_manager::start).get();

    syschecks::systemd_message("Starting Kafka group manager").get();
    _group_manager.invoke_on_all(&kafka::group_manager::start).get();

    syschecks::systemd_message("Starting controller").get();
    controller->start().get0();
    /**
     * We schedule shutting down controller input and aborting its operation
     * as a first shutdown step. (other services are stopeed in
     * an order reverse to the startup sequence.) This way we terminate all long
     * running opertions before shutting down the RPC server, preventing it to
     * wait on background dispatch gate `close` call.
     *
     * NOTE controller has to be stopped only after it was started
     */
    _deferred.emplace_back([this] { controller->shutdown_input().get(); });
    // FIXME: in first patch explain why this is started after the
    // controller so the broker set will be available. Then next patch fix.
    syschecks::systemd_message("Starting metadata dissination service").get();
    md_dissemination_service
      .invoke_on_all(&cluster::metadata_dissemination_service::start)
      .get();

    syschecks::systemd_message("Starting RPC").get();
    _rpc
      .invoke_on_all([this](rpc::server& s) {
          auto proto = std::make_unique<rpc::simple_protocol>();
          proto->register_service<cluster::id_allocator>(
            _scheduling_groups.raft_sg(),
            smp_service_groups.raft_smp_sg(),
            std::ref(id_allocator_frontend));
          proto->register_service<
            raft::service<cluster::partition_manager, cluster::shard_table>>(
            _scheduling_groups.raft_sg(),
            smp_service_groups.raft_smp_sg(),
            partition_manager,
            shard_table.local(),
            config::shard_local_cfg().raft_heartbeat_interval_ms());
          proto->register_service<cluster::service>(
            _scheduling_groups.cluster_sg(),
            smp_service_groups.cluster_smp_sg(),
            std::ref(controller->get_topics_frontend()),
            std::ref(controller->get_members_manager()),
            std::ref(metadata_cache),
            std::ref(controller->get_security_frontend()));
          proto->register_service<cluster::metadata_dissemination_handler>(
            _scheduling_groups.cluster_sg(),
            smp_service_groups.cluster_smp_sg(),
            std::ref(controller->get_partition_leaders()));
          s.set_protocol(std::move(proto));
      })
      .get();
    auto& conf = config::shard_local_cfg();
    _rpc.invoke_on_all(&rpc::server::start).get();
    vlog(_log.info, "Started RPC server listening at {}", conf.rpc_server());

    if (archival_storage_enabled()) {
        syschecks::systemd_message("Starting archival storage").get();
        archival_scheduler
          .invoke_on_all(
            [](archival::scheduler_service& svc) { return svc.start(); })
          .get();
    }

    quota_mgr.invoke_on_all(&kafka::quota_manager::start).get();

    // Kafka API
    _kafka_server
      .invoke_on_all([this](rpc::server& s) {
          auto proto = std::make_unique<kafka::protocol>(
            smp_service_groups.kafka_smp_sg(),
            metadata_cache,
            controller->get_topics_frontend(),
            quota_mgr,
            group_router,
            shard_table,
            partition_manager,
            coordinator_ntp_mapper,
            fetch_session_cache,
            std::ref(id_allocator_frontend),
            controller->get_credential_store(),
            controller->get_authorizer(),
            controller->get_security_frontend());
          s.set_protocol(std::move(proto));
      })
      .get();
    _kafka_server.invoke_on_all(&rpc::server::start).get();
    vlog(
      _log.info, "Started Kafka API server listening at {}", conf.kafka_api());

    if (coproc_enabled()) {
        construct_single_service(_wasm_event_listener, std::ref(pacemaker));
        _wasm_event_listener->start().get();
        pacemaker.invoke_on_all(&coproc::pacemaker::start).get();
    }
}

void application::admin_register_raft_routes(ss::http_server& server) {
    ss::httpd::raft_json::raft_transfer_leadership.set(
      server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          raft::group_id group_id;
          try {
              group_id = raft::group_id(std::stoll(req->param["group_id"]));
          } catch (...) {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Raft group id must be an integer: {}",
                req->param["group_id"]));
          }

          if (group_id() < 0) {
              throw ss::httpd::bad_param_exception(
                fmt::format("Invalid raft group id {}", group_id));
          }

          if (!shard_table.local().contains(group_id)) {
              throw ss::httpd::not_found_exception(
                fmt::format("Raft group {} not found", group_id));
          }

          std::optional<model::node_id> target;
          if (auto node = req->get_query_param("target"); !node.empty()) {
              try {
                  target = model::node_id(std::stoi(node));
              } catch (...) {
                  throw ss::httpd::bad_param_exception(
                    fmt::format("Target node id must be an integer: {}", node));
              }
              if (*target < 0) {
                  throw ss::httpd::bad_param_exception(
                    fmt::format("Invalid target node id {}", *target));
              }
          }

          vlog(
            _log.info,
            "Leadership transfer request for raft group {} to node {}",
            group_id,
            target);

          auto shard = shard_table.local().shard_for(group_id);

          return partition_manager.invoke_on(
            shard, [group_id, target](cluster::partition_manager& pm) mutable {
                auto consensus = pm.consensus_for(group_id);
                if (!consensus) {
                    throw ss::httpd::not_found_exception();
                }
                return consensus->transfer_leadership(target).then(
                  [](std::error_code err) {
                      if (err) {
                          throw ss::httpd::server_error_exception(fmt::format(
                            "Leadership transfer failed: {}", err.message()));
                      }
                      return ss::json::json_return_type(ss::json::json_void());
                  });
            });
      });
}

/*
 * Parse integer pairs from: ?target={\d,\d}* where each pair represent a
 * node-id and a shard-id, repsectively.
 */
static std::vector<model::broker_shard>
parse_target_broker_shards(const ss::sstring& param) {
    std::vector<ss::sstring> parts;
    boost::split(parts, param, boost::is_any_of(","));

    if (parts.size() % 2 != 0) {
        throw ss::httpd::bad_param_exception(
          fmt::format("Invalid target parameter format: {}", param));
    }

    std::vector<model::broker_shard> replicas;

    for (auto i = 0u; i < parts.size(); i += 2) {
        auto node = std::stoi(parts[i]);
        auto shard = std::stoi(parts[i + 1]);

        if (node < 0 || shard < 0) {
            throw ss::httpd::bad_param_exception(
              fmt::format("Invalid target {}:{}", node, shard));
        }

        replicas.push_back(model::broker_shard{
          .node_id = model::node_id(node),
          .shard = static_cast<uint32_t>(shard),
        });
    }

    return replicas;
}

// TODO: factor out generic serialization from seastar http exceptions
static security::scram_credential
parse_scram_credential(const rapidjson::Document& doc) {
    if (!doc.IsObject()) {
        throw ss::httpd::bad_request_exception(fmt::format("Not an object"));
    }

    if (!doc.HasMember("algorithm") || !doc["algorithm"].IsString()) {
        throw ss::httpd::bad_request_exception(
          fmt::format("String algo missing"));
    }
    const auto algorithm = std::string_view(
      doc["algorithm"].GetString(), doc["algorithm"].GetStringLength());

    if (!doc.HasMember("password") || !doc["password"].IsString()) {
        throw ss::httpd::bad_request_exception(
          fmt::format("String password smissing"));
    }
    const auto password = doc["password"].GetString();

    security::scram_credential credential;

    if (algorithm == security::scram_sha256_authenticator::name) {
        credential = security::scram_sha256::make_credentials(
          password, security::scram_sha256::min_iterations);

    } else if (algorithm == security::scram_sha512_authenticator::name) {
        credential = security::scram_sha512::make_credentials(
          password, security::scram_sha512::min_iterations);

    } else {
        throw ss::httpd::bad_request_exception(
          fmt::format("Unknown scram algorithm: {}", algorithm));
    }

    return credential;
}

void application::admin_register_security_routes(ss::http_server& server) {
    ss::httpd::security_json::create_user.set(
      server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          rapidjson::Document doc;
          doc.Parse(req->content.data());

          auto credential = parse_scram_credential(doc);

          if (!doc.HasMember("username") || !doc["username"].IsString()) {
              throw ss::httpd::bad_request_exception(
                fmt::format("String username missing"));
          }

          auto username = security::credential_user(
            doc["username"].GetString());

          return controller->get_security_frontend()
            .local()
            .create_user(username, credential, model::timeout_clock::now() + 5s)
            .then([this](std::error_code err) {
                vlog(_log.debug, "Creating user {}:{}", err, err.message());
                if (err) {
                    throw ss::httpd::bad_request_exception(
                      fmt::format("Creating user: {}", err.message()));
                }
                return ss::make_ready_future<ss::json::json_return_type>(
                  ss::json::json_return_type(ss::json::json_void()));
            });
      });

    ss::httpd::security_json::delete_user.set(
      server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          auto user = security::credential_user(
            model::topic(req->param["user"]));

          return controller->get_security_frontend()
            .local()
            .delete_user(user, model::timeout_clock::now() + 5s)
            .then([this](std::error_code err) {
                vlog(_log.debug, "Deleting user {}:{}", err, err.message());
                if (err) {
                    throw ss::httpd::bad_request_exception(
                      fmt::format("Deleting user: {}", err.message()));
                }
                return ss::make_ready_future<ss::json::json_return_type>(
                  ss::json::json_return_type(ss::json::json_void()));
            });
      });

    ss::httpd::security_json::update_user.set(
      server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          auto user = security::credential_user(
            model::topic(req->param["user"]));

          rapidjson::Document doc;
          doc.Parse(req->content.data());

          auto credential = parse_scram_credential(doc);

          return controller->get_security_frontend()
            .local()
            .update_user(user, credential, model::timeout_clock::now() + 5s)
            .then([this](std::error_code err) {
                vlog(_log.debug, "Updating user {}:{}", err, err.message());
                if (err) {
                    throw ss::httpd::bad_request_exception(
                      fmt::format("Updating user: {}", err.message()));
                }
                return ss::make_ready_future<ss::json::json_return_type>(
                  ss::json::json_return_type(ss::json::json_void()));
            });
      });

    ss::httpd::security_json::list_users.set(
      server._routes, [this](std::unique_ptr<ss::httpd::request>) {
          std::vector<ss::sstring> users;
          for (const auto& [user, _] :
               controller->get_credential_store().local()) {
              users.push_back(user());
          }
          return ss::make_ready_future<ss::json::json_return_type>(
            std::move(users));
      });
}

void application::admin_register_kafka_routes(ss::http_server& server) {
    ss::httpd::kafka_json::kafka_transfer_leadership.set(
      server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          auto topic = model::topic(req->param["topic"]);

          model::partition_id partition;
          try {
              partition = model::partition_id(
                std::stoll(req->param["partition"]));
          } catch (...) {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Partition id must be an integer: {}",
                req->param["partition"]));
          }

          if (partition() < 0) {
              throw ss::httpd::bad_param_exception(
                fmt::format("Invalid partition id {}", partition));
          }

          std::optional<model::node_id> target;
          if (auto node = req->get_query_param("target"); !node.empty()) {
              try {
                  target = model::node_id(std::stoi(node));
              } catch (...) {
                  throw ss::httpd::bad_param_exception(
                    fmt::format("Target node id must be an integer: {}", node));
              }
              if (*target < 0) {
                  throw ss::httpd::bad_param_exception(
                    fmt::format("Invalid target node id {}", *target));
              }
          }

          vlog(
            _log.info,
            "Leadership transfer request for leader of topic-partition {}:{} "
            "to node {}",
            topic,
            partition,
            target);

          model::ntp ntp(model::kafka_namespace, topic, partition);

          auto shard = shard_table.local().shard_for(ntp);
          if (!shard) {
              throw ss::httpd::not_found_exception(fmt::format(
                "Topic partition {}:{} not found", topic, partition));
          }

          return partition_manager.invoke_on(
            *shard,
            [ntp = std::move(ntp),
             target](cluster::partition_manager& pm) mutable {
                auto partition = pm.get(ntp);
                if (!partition) {
                    throw ss::httpd::not_found_exception();
                }
                return partition->transfer_leadership(target).then(
                  [](std::error_code err) {
                      if (err) {
                          throw ss::httpd::server_error_exception(fmt::format(
                            "Leadership transfer failed: {}", err.message()));
                      }
                      return ss::json::json_return_type(ss::json::json_void());
                  });
            });
      });

    ss::httpd::partition_json::kafka_move_partition.set(
      server._routes, [this](std::unique_ptr<ss::httpd::request> req) {
          auto topic = model::topic(req->param["topic"]);

          model::partition_id partition;
          try {
              partition = model::partition_id(
                std::stoll(req->param["partition"]));
          } catch (...) {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Partition id must be an integer: {}",
                req->param["partition"]));
          }

          if (partition() < 0) {
              throw ss::httpd::bad_param_exception(
                fmt::format("Invalid partition id {}", partition));
          }

          std::optional<std::vector<model::broker_shard>> replicas;
          if (auto node = req->get_query_param("target"); !node.empty()) {
              try {
                  replicas = parse_target_broker_shards(node);
              } catch (...) {
                  throw ss::httpd::bad_param_exception(fmt::format(
                    "Invalid target format {}: {}",
                    node,
                    std::current_exception()));
              }
          }

          // this can be removed when we have more sophisticated machinary in
          // redpanda itself for automatically selecting target node/shard.
          if (!replicas || replicas->empty()) {
              throw ss::httpd::bad_request_exception(
                "Partition movement requires target replica set");
          }

          model::ntp ntp(model::kafka_namespace, topic, partition);

          vlog(
            _log.debug,
            "Request to change ntp {} replica set to {}",
            ntp,
            replicas);

          return controller->get_topics_frontend()
            .local()
            .move_partition_replicas(
              ntp, *replicas, model::timeout_clock::now() + 5s)
            .then([this, ntp, replicas](std::error_code err) {
                vlog(
                  _log.debug,
                  "Result changing ntp {} replica set to {}: {}:{}",
                  ntp,
                  replicas,
                  err,
                  err.message());
                if (err) {
                    throw ss::httpd::bad_request_exception(
                      fmt::format("Error moving partition: {}", err.message()));
                }
                return ss::make_ready_future<ss::json::json_return_type>(
                  ss::json::json_return_type(ss::json::json_void()));
            });
      });
}
