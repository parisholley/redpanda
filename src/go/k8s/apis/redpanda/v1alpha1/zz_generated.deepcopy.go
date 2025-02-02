// +build !ignore_autogenerated

// Copyright 2021 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

// Code generated by controller-gen. DO NOT EDIT.

package v1alpha1

import (
	metav1 "github.com/jetstack/cert-manager/pkg/apis/meta/v1"
	"k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/runtime"
)

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *AdminAPITLS) DeepCopyInto(out *AdminAPITLS) {
	*out = *in
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new AdminAPITLS.
func (in *AdminAPITLS) DeepCopy() *AdminAPITLS {
	if in == nil {
		return nil
	}
	out := new(AdminAPITLS)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *CloudStorageConfig) DeepCopyInto(out *CloudStorageConfig) {
	*out = *in
	out.SecretKeyRef = in.SecretKeyRef
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new CloudStorageConfig.
func (in *CloudStorageConfig) DeepCopy() *CloudStorageConfig {
	if in == nil {
		return nil
	}
	out := new(CloudStorageConfig)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *Cluster) DeepCopyInto(out *Cluster) {
	*out = *in
	out.TypeMeta = in.TypeMeta
	in.ObjectMeta.DeepCopyInto(&out.ObjectMeta)
	in.Spec.DeepCopyInto(&out.Spec)
	in.Status.DeepCopyInto(&out.Status)
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new Cluster.
func (in *Cluster) DeepCopy() *Cluster {
	if in == nil {
		return nil
	}
	out := new(Cluster)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyObject is an autogenerated deepcopy function, copying the receiver, creating a new runtime.Object.
func (in *Cluster) DeepCopyObject() runtime.Object {
	if c := in.DeepCopy(); c != nil {
		return c
	}
	return nil
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *ClusterList) DeepCopyInto(out *ClusterList) {
	*out = *in
	out.TypeMeta = in.TypeMeta
	in.ListMeta.DeepCopyInto(&out.ListMeta)
	if in.Items != nil {
		in, out := &in.Items, &out.Items
		*out = make([]Cluster, len(*in))
		for i := range *in {
			(*in)[i].DeepCopyInto(&(*out)[i])
		}
	}
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new ClusterList.
func (in *ClusterList) DeepCopy() *ClusterList {
	if in == nil {
		return nil
	}
	out := new(ClusterList)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyObject is an autogenerated deepcopy function, copying the receiver, creating a new runtime.Object.
func (in *ClusterList) DeepCopyObject() runtime.Object {
	if c := in.DeepCopy(); c != nil {
		return c
	}
	return nil
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *ClusterSpec) DeepCopyInto(out *ClusterSpec) {
	*out = *in
	if in.Replicas != nil {
		in, out := &in.Replicas, &out.Replicas
		*out = new(int32)
		**out = **in
	}
	in.Resources.DeepCopyInto(&out.Resources)
	in.Configuration.DeepCopyInto(&out.Configuration)
	if in.Tolerations != nil {
		in, out := &in.Tolerations, &out.Tolerations
		*out = make([]v1.Toleration, len(*in))
		for i := range *in {
			(*in)[i].DeepCopyInto(&(*out)[i])
		}
	}
	if in.NodeSelector != nil {
		in, out := &in.NodeSelector, &out.NodeSelector
		*out = make(map[string]string, len(*in))
		for key, val := range *in {
			(*out)[key] = val
		}
	}
	out.ExternalConnectivity = in.ExternalConnectivity
	in.Storage.DeepCopyInto(&out.Storage)
	out.CloudStorage = in.CloudStorage
	if in.Superusers != nil {
		in, out := &in.Superusers, &out.Superusers
		*out = make([]Superuser, len(*in))
		copy(*out, *in)
	}
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new ClusterSpec.
func (in *ClusterSpec) DeepCopy() *ClusterSpec {
	if in == nil {
		return nil
	}
	out := new(ClusterSpec)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *ClusterStatus) DeepCopyInto(out *ClusterStatus) {
	*out = *in
	in.Nodes.DeepCopyInto(&out.Nodes)
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new ClusterStatus.
func (in *ClusterStatus) DeepCopy() *ClusterStatus {
	if in == nil {
		return nil
	}
	out := new(ClusterStatus)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *ExternalConnectivityConfig) DeepCopyInto(out *ExternalConnectivityConfig) {
	*out = *in
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new ExternalConnectivityConfig.
func (in *ExternalConnectivityConfig) DeepCopy() *ExternalConnectivityConfig {
	if in == nil {
		return nil
	}
	out := new(ExternalConnectivityConfig)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *KafkaAPITLS) DeepCopyInto(out *KafkaAPITLS) {
	*out = *in
	if in.IssuerRef != nil {
		in, out := &in.IssuerRef, &out.IssuerRef
		*out = new(metav1.ObjectReference)
		**out = **in
	}
	if in.NodeSecretRef != nil {
		in, out := &in.NodeSecretRef, &out.NodeSecretRef
		*out = new(v1.ObjectReference)
		**out = **in
	}
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new KafkaAPITLS.
func (in *KafkaAPITLS) DeepCopy() *KafkaAPITLS {
	if in == nil {
		return nil
	}
	out := new(KafkaAPITLS)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *NodesList) DeepCopyInto(out *NodesList) {
	*out = *in
	if in.Internal != nil {
		in, out := &in.Internal, &out.Internal
		*out = make([]string, len(*in))
		copy(*out, *in)
	}
	if in.External != nil {
		in, out := &in.External, &out.External
		*out = make([]string, len(*in))
		copy(*out, *in)
	}
	if in.ExternalAdmin != nil {
		in, out := &in.ExternalAdmin, &out.ExternalAdmin
		*out = make([]string, len(*in))
		copy(*out, *in)
	}
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new NodesList.
func (in *NodesList) DeepCopy() *NodesList {
	if in == nil {
		return nil
	}
	out := new(NodesList)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *RedpandaConfig) DeepCopyInto(out *RedpandaConfig) {
	*out = *in
	out.RPCServer = in.RPCServer
	out.KafkaAPI = in.KafkaAPI
	out.AdminAPI = in.AdminAPI
	in.TLS.DeepCopyInto(&out.TLS)
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new RedpandaConfig.
func (in *RedpandaConfig) DeepCopy() *RedpandaConfig {
	if in == nil {
		return nil
	}
	out := new(RedpandaConfig)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *SocketAddress) DeepCopyInto(out *SocketAddress) {
	*out = *in
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new SocketAddress.
func (in *SocketAddress) DeepCopy() *SocketAddress {
	if in == nil {
		return nil
	}
	out := new(SocketAddress)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *StorageSpec) DeepCopyInto(out *StorageSpec) {
	*out = *in
	out.Capacity = in.Capacity.DeepCopy()
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new StorageSpec.
func (in *StorageSpec) DeepCopy() *StorageSpec {
	if in == nil {
		return nil
	}
	out := new(StorageSpec)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *Superuser) DeepCopyInto(out *Superuser) {
	*out = *in
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new Superuser.
func (in *Superuser) DeepCopy() *Superuser {
	if in == nil {
		return nil
	}
	out := new(Superuser)
	in.DeepCopyInto(out)
	return out
}

// DeepCopyInto is an autogenerated deepcopy function, copying the receiver, writing into out. in must be non-nil.
func (in *TLSConfig) DeepCopyInto(out *TLSConfig) {
	*out = *in
	in.KafkaAPI.DeepCopyInto(&out.KafkaAPI)
	out.AdminAPI = in.AdminAPI
}

// DeepCopy is an autogenerated deepcopy function, copying the receiver, creating a new TLSConfig.
func (in *TLSConfig) DeepCopy() *TLSConfig {
	if in == nil {
		return nil
	}
	out := new(TLSConfig)
	in.DeepCopyInto(out)
	return out
}
