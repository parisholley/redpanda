// Copyright 2021 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package certmanager

import (
	cmmeta "github.com/jetstack/cert-manager/pkg/apis/meta/v1"
	"github.com/vectorizedio/redpanda/src/go/k8s/pkg/resources"
	"k8s.io/apimachinery/pkg/types"
)

const (
	adminAPI = "admin"
	// AdminAPIClientCert cert name - client certificate for Admin API
	AdminAPIClientCert = "admin-api-client"
	// AdminAPINodeCert cert name - node certificate for Admin API
	AdminAPINodeCert = "admin-api-node"
)

// AdminAPINodeCert returns the namespaced name for the Admin API certificate used by node
func (r *PkiReconciler) AdminAPINodeCert() types.NamespacedName {
	return types.NamespacedName{Name: r.pandaCluster.Name + "-" + AdminAPINodeCert, Namespace: r.pandaCluster.Namespace}
}

func (r *PkiReconciler) prepareAdminAPI(
	issuerRef *cmmeta.ObjectReference,
) []resources.Resource {
	toApply := []resources.Resource{}

	// Redpanda cluster certificate for Admin API - to be provided to each broker
	cn := NewCommonName(r.pandaCluster.Name, AdminAPINodeCert)
	certsKey := types.NamespacedName{Name: string(cn), Namespace: r.pandaCluster.Namespace}

	dnsName := r.internalFQDN
	externConn := r.pandaCluster.Spec.ExternalConnectivity
	if externConn.Enabled && externConn.Subdomain != "" {
		dnsName = externConn.Subdomain
	}

	nodeCert := NewNodeCertificate(r.Client, r.scheme, r.pandaCluster, certsKey, issuerRef, dnsName, cn, false, r.logger)
	toApply = append(toApply, nodeCert)

	if r.pandaCluster.Spec.Configuration.TLS.AdminAPI.RequireClientAuth {
		// Certificate for calling the Admin API on any broker
		cn := NewCommonName(r.pandaCluster.Name, AdminAPIClientCert)
		clientCertsKey := types.NamespacedName{Name: string(cn), Namespace: r.pandaCluster.Namespace}
		adminClientCert := NewCertificate(r.Client, r.scheme, r.pandaCluster, clientCertsKey, issuerRef, cn, false, r.logger)

		toApply = append(toApply, adminClientCert)
	}

	return toApply
}
