#include "extensions/filters/network/redis_proxy/conn_pool_impl.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace RedisProxy {
namespace ConnPool {

InstanceImpl::InstanceImpl(
    const std::string& cluster_name, Upstream::ClusterManager& cm,
    Common::Redis::Client::ClientFactory& client_factory, ThreadLocal::SlotAllocator& tls,
    const envoy::config::filter::network::redis_proxy::v2::RedisProxy::ConnPoolSettings& config)
    : cm_(cm), client_factory_(client_factory), tls_(tls.allocateSlot()), config_(config) {
  tls_->set([this, cluster_name](
                Event::Dispatcher& dispatcher) -> ThreadLocal::ThreadLocalObjectSharedPtr {
    return std::make_shared<ThreadLocalPool>(*this, dispatcher, cluster_name);
  });
}

Common::Redis::Client::PoolRequest*
InstanceImpl::makeRequest(const std::string& key, const Common::Redis::RespValue& request,
                          Common::Redis::Client::PoolCallbacks& callbacks) {
  return tls_->getTyped<ThreadLocalPool>().makeRequest(key, request, callbacks);
}

Common::Redis::Client::PoolRequest*
InstanceImpl::makeRequestToHost(const std::string& host_address,
                                const Common::Redis::RespValue& request,
                                Common::Redis::Client::PoolCallbacks& callbacks) {
  return tls_->getTyped<ThreadLocalPool>().makeRequestToHost(host_address, request, callbacks);
}

InstanceImpl::ThreadLocalPool::ThreadLocalPool(InstanceImpl& parent, Event::Dispatcher& dispatcher,
                                               std::string cluster_name)
    : parent_(parent), dispatcher_(dispatcher), cluster_name_(std::move(cluster_name)) {

  cluster_update_handle_ = parent_.cm_.addThreadLocalClusterUpdateCallbacks(*this);
  Upstream::ThreadLocalCluster* cluster = parent_.cm_.get(cluster_name_);
  if (cluster != nullptr) {
    onClusterAddOrUpdateNonVirtual(*cluster);
  }
}

InstanceImpl::ThreadLocalPool::~ThreadLocalPool() {
  if (host_set_member_update_cb_handle_ != nullptr) {
    host_set_member_update_cb_handle_->remove();
  }
  while (!client_map_.empty()) {
    client_map_.begin()->second->redis_client_->close();
  }
}

void InstanceImpl::ThreadLocalPool::onClusterAddOrUpdateNonVirtual(
    Upstream::ThreadLocalCluster& cluster) {
  if (cluster.info()->name() != cluster_name_) {
    return;
  }

  if (cluster_ != nullptr) {
    // Treat an update as a removal followed by an add.
    onClusterRemoval(cluster_name_);
  }

  ASSERT(cluster_ == nullptr);
  cluster_ = &cluster;
  ASSERT(host_set_member_update_cb_handle_ == nullptr);
  host_set_member_update_cb_handle_ = cluster_->prioritySet().addMemberUpdateCb(
      [this](const std::vector<Upstream::HostSharedPtr>&,
             const std::vector<Upstream::HostSharedPtr>& hosts_removed) -> void {
        onHostsRemoved(hosts_removed);
      });

  ASSERT(host_address_map_.empty());
  for (uint32_t i = 0; i < cluster_->prioritySet().hostSetsPerPriority().size(); i++) {
    for (auto& host : cluster_->prioritySet().hostSetsPerPriority()[i]->hosts()) {
      host_address_map_[host->address()->asString()] = host;
    }
  }
}

void InstanceImpl::ThreadLocalPool::onClusterRemoval(const std::string& cluster_name) {
  if (cluster_name != cluster_name_) {
    return;
  }

  // Treat cluster removal as a removal of all hosts. Close all connections and fail all pending
  // requests.
  while (!client_map_.empty()) {
    client_map_.begin()->second->redis_client_->close();
  }

  cluster_ = nullptr;
  host_set_member_update_cb_handle_ = nullptr;
  host_address_map_.clear();
}

void InstanceImpl::ThreadLocalPool::onHostsRemoved(
    const std::vector<Upstream::HostSharedPtr>& hosts_removed) {
  for (const auto& host : hosts_removed) {
    auto it = client_map_.find(host);
    if (it != client_map_.end()) {
      // We don't currently support any type of draining for redis connections. If a host is gone,
      // we just close the connection. This will fail any pending requests.
      it->second->redis_client_->close();
    }
    host_address_map_.erase(host->address()->asString());
  }
}

Common::Redis::Client::PoolRequest*
InstanceImpl::ThreadLocalPool::makeRequest(const std::string& key,
                                           const Common::Redis::RespValue& request,
                                           Common::Redis::Client::PoolCallbacks& callbacks) {
  if (cluster_ == nullptr) {
    ASSERT(client_map_.empty());
    ASSERT(host_set_member_update_cb_handle_ == nullptr);
    return nullptr;
  }

  LbContextImpl lb_context(key, parent_.config_.enableHashtagging());
  Upstream::HostConstSharedPtr host = cluster_->loadBalancer().chooseHost(&lb_context);
  if (!host) {
    return nullptr;
  }

  ThreadLocalActiveClientPtr& client = client_map_[host];
  if (!client) {
    client = std::make_unique<ThreadLocalActiveClient>(*this);
    client->host_ = host;
    client->redis_client_ = parent_.client_factory_.create(host, dispatcher_, parent_.config_);
    client->redis_client_->addConnectionCallbacks(*client);
  }

  // Keep host_address_map_ in sync with client_map_.
  auto host_cached_by_address = host_address_map_.find(host->address()->asString());
  if (host_cached_by_address == host_address_map_.end()) {
    host_address_map_[host->address()->asString()] = host;
  }

  return client->redis_client_->makeRequest(request, callbacks);
}

Common::Redis::Client::PoolRequest*
InstanceImpl::ThreadLocalPool::makeRequestToHost(const std::string& host_address,
                                                 const Common::Redis::RespValue& request,
                                                 Common::Redis::Client::PoolCallbacks& callbacks) {
  if (cluster_ == nullptr) {
    ASSERT(client_map_.empty());
    ASSERT(host_set_member_update_cb_handle_ == nullptr);
    return nullptr;
  }

  auto colon_pos = host_address.rfind(":");
  if ((colon_pos == std::string::npos) || (colon_pos == (host_address.size() - 1))) {
    return nullptr;
  }

  const std::string ip_address = host_address.substr(0, colon_pos);
  const bool ipv6 = (ip_address.find(":") != std::string::npos);
  std::string host_address_map_key;
  Network::Address::InstanceConstSharedPtr address_ptr;

  if (!ipv6) {
    host_address_map_key = host_address;
  } else {
    const auto ip_port = absl::string_view(host_address).substr(colon_pos + 1);
    uint64_t ip_port_number;
    if (!absl::SimpleAtoi(ip_port, &ip_port_number) || (ip_port_number > 65535)) {
      return nullptr;
    }
    try {
      address_ptr = std::make_shared<Network::Address::Ipv6Instance>(ip_address, ip_port_number);
    } catch (const EnvoyException&) {
      return nullptr;
    }
    host_address_map_key = address_ptr->asString();
  }

  auto it = host_address_map_.find(host_address_map_key);
  if (it == host_address_map_.end()) {
    // This host is not known to the cluster manager. Create a new host and insert it into the map.
    // TODO(msukalski): Add logic to track the number of these "unknown" host connections,
    // cap the number of these connections, and implement time-out and cleaning logic, etc.

    if (!ipv6) {
      // Only create an IPv4 address instance if we need a new Upstream::HostImpl.
      const auto ip_port = absl::string_view(host_address).substr(colon_pos + 1);
      uint64_t ip_port_number;
      if (!absl::SimpleAtoi(ip_port, &ip_port_number) || (ip_port_number > 65535)) {
        return nullptr;
      }
      try {
        address_ptr = std::make_shared<Network::Address::Ipv4Instance>(ip_address, ip_port_number);
      } catch (const EnvoyException&) {
        return nullptr;
      }
    }
    Upstream::HostSharedPtr new_host{new Upstream::HostImpl(
        cluster_->info(), "", address_ptr, envoy::api::v2::core::Metadata::default_instance(), 1,
        envoy::api::v2::core::Locality(),
        envoy::api::v2::endpoint::Endpoint::HealthCheckConfig::default_instance(), 0,
        envoy::api::v2::core::HealthStatus::UNKNOWN)};
    host_address_map_[host_address_map_key] = new_host;
    it = host_address_map_.find(host_address_map_key);
  }

  ThreadLocalActiveClientPtr& client = client_map_[it->second];
  if (!client) {
    client = std::make_unique<ThreadLocalActiveClient>(*this);
    client->host_ = it->second;
    client->redis_client_ =
        parent_.client_factory_.create(it->second, dispatcher_, parent_.config_);
    client->redis_client_->addConnectionCallbacks(*client);
  }

  return client->redis_client_->makeRequest(request, callbacks);
}

void InstanceImpl::ThreadLocalActiveClient::onEvent(Network::ConnectionEvent event) {
  if (event == Network::ConnectionEvent::RemoteClose ||
      event == Network::ConnectionEvent::LocalClose) {
    auto client_to_delete = parent_.client_map_.find(host_);
    ASSERT(client_to_delete != parent_.client_map_.end());
    parent_.dispatcher_.deferredDelete(std::move(client_to_delete->second->redis_client_));
    parent_.client_map_.erase(client_to_delete);
  }
}

// Inspired by the redis-cluster hashtagging algorithm
// https://redis.io/topics/cluster-spec#keys-hash-tags
absl::string_view InstanceImpl::LbContextImpl::hashtag(absl::string_view v, bool enabled) {
  if (!enabled) {
    return v;
  }

  auto start = v.find('{');
  if (start == std::string::npos) {
    return v;
  }

  auto end = v.find('}', start);
  if (end == std::string::npos || end == start + 1) {
    return v;
  }

  return v.substr(start + 1, end - start - 1);
}

} // namespace ConnPool
} // namespace RedisProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
