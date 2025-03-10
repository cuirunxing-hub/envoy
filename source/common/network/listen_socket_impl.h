#pragma once

#include <memory>
#include <string>
#include <vector>

#include "envoy/common/platform.h"
#include "envoy/network/connection.h"
#include "envoy/network/listen_socket.h"
#include "envoy/network/socket.h"
#include "envoy/network/socket_interface.h"

#include "common/common/assert.h"
#include "common/common/dump_state_utils.h"
#include "common/network/socket_impl.h"
#include "common/network/socket_interface.h"

namespace Envoy {
namespace Network {

class ListenSocketImpl : public SocketImpl {
protected:
  ListenSocketImpl(IoHandlePtr&& io_handle, const Address::InstanceConstSharedPtr& local_address)
      : SocketImpl(std::move(io_handle), local_address, nullptr) {}

  SocketPtr duplicate() override {
    // Using `new` to access a non-public constructor.
    return absl::WrapUnique(
        new ListenSocketImpl(io_handle_ == nullptr ? nullptr : io_handle_->duplicate(),
                             address_provider_->localAddress()));
  }

  void setupSocket(const Network::Socket::OptionsSharedPtr& options, bool bind_to_port);
  void setListenSocketOptions(const Network::Socket::OptionsSharedPtr& options);
  Api::SysCallIntResult bind(Network::Address::InstanceConstSharedPtr address) override;
};

/**
 * Wraps a unix socket.
 */
template <Socket::Type T> struct NetworkSocketTrait {};

template <> struct NetworkSocketTrait<Socket::Type::Stream> {
  static constexpr Socket::Type type = Socket::Type::Stream;
};

template <> struct NetworkSocketTrait<Socket::Type::Datagram> {
  static constexpr Socket::Type type = Socket::Type::Datagram;
};

template <typename T> class NetworkListenSocket : public ListenSocketImpl {
public:
  NetworkListenSocket(const Address::InstanceConstSharedPtr& address,
                      const Network::Socket::OptionsSharedPtr& options, bool bind_to_port)
      : ListenSocketImpl(bind_to_port ? Network::ioHandleForAddr(T::type, address) : nullptr,
                         address) {
    // Prebind is applied if the socket is bind to port.
    if (bind_to_port) {
      RELEASE_ASSERT(io_handle_->isOpen(), "");
      setPrebindSocketOptions();
    } else {
      // If the tcp listener does not bind to port, we test that the ip family is supported.
      if (auto ip = address->ip(); ip != nullptr) {
        RELEASE_ASSERT(
            Network::SocketInterfaceSingleton::get().ipFamilySupported(ip->ipv4() ? AF_INET
                                                                                  : AF_INET6),
            fmt::format(
                "Creating listen socket address {} but the address familiy is not supported",
                address->asStringView()));
      }
    }
    setupSocket(options, bind_to_port);
  }

  NetworkListenSocket(IoHandlePtr&& io_handle, const Address::InstanceConstSharedPtr& address,
                      const Network::Socket::OptionsSharedPtr& options)
      : ListenSocketImpl(std::move(io_handle), address) {
    setListenSocketOptions(options);
  }

  Socket::Type socketType() const override { return T::type; }

protected:
  void setPrebindSocketOptions() {
    // On Windows, SO_REUSEADDR does not restrict subsequent bind calls when there is a listener as
    // on Linux and later BSD socket stacks
#ifndef WIN32
    int on = 1;
    auto status = setSocketOption(SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    RELEASE_ASSERT(status.rc_ != -1, "failed to set SO_REUSEADDR socket option");
#endif
  }
};

template <>
inline void
NetworkListenSocket<NetworkSocketTrait<Socket::Type::Datagram>>::setPrebindSocketOptions() {}

// UDP listen socket desires io handle regardless bind_to_port is true or false.
template <>
NetworkListenSocket<NetworkSocketTrait<Socket::Type::Datagram>>::NetworkListenSocket(
    const Address::InstanceConstSharedPtr& address,
    const Network::Socket::OptionsSharedPtr& options, bool bind_to_port);

template class NetworkListenSocket<NetworkSocketTrait<Socket::Type::Stream>>;
template class NetworkListenSocket<NetworkSocketTrait<Socket::Type::Datagram>>;

using TcpListenSocket = NetworkListenSocket<NetworkSocketTrait<Socket::Type::Stream>>;
using TcpListenSocketPtr = std::unique_ptr<TcpListenSocket>;

using UdpListenSocket = NetworkListenSocket<NetworkSocketTrait<Socket::Type::Datagram>>;
using UdpListenSocketPtr = std::unique_ptr<UdpListenSocket>;

class UdsListenSocket : public ListenSocketImpl {
public:
  UdsListenSocket(const Address::InstanceConstSharedPtr& address);
  UdsListenSocket(IoHandlePtr&& io_handle, const Address::InstanceConstSharedPtr& address);
  Socket::Type socketType() const override { return Socket::Type::Stream; }
};

class ConnectionSocketImpl : public SocketImpl, public ConnectionSocket {
public:
  ConnectionSocketImpl(IoHandlePtr&& io_handle,
                       const Address::InstanceConstSharedPtr& local_address,
                       const Address::InstanceConstSharedPtr& remote_address)
      : SocketImpl(std::move(io_handle), local_address, remote_address) {}

  ConnectionSocketImpl(Socket::Type type, const Address::InstanceConstSharedPtr& local_address,
                       const Address::InstanceConstSharedPtr& remote_address)
      : SocketImpl(type, local_address, remote_address) {
    address_provider_->setLocalAddress(local_address);
  }

  // Network::Socket
  Socket::Type socketType() const override { return Socket::Type::Stream; }

  // Network::ConnectionSocket
  void setDetectedTransportProtocol(absl::string_view protocol) override {
    transport_protocol_ = std::string(protocol);
  }
  absl::string_view detectedTransportProtocol() const override { return transport_protocol_; }

  void setRequestedApplicationProtocols(const std::vector<absl::string_view>& protocols) override {
    application_protocols_.clear();
    for (const auto& protocol : protocols) {
      application_protocols_.emplace_back(protocol);
    }
  }
  const std::vector<std::string>& requestedApplicationProtocols() const override {
    return application_protocols_;
  }

  void setRequestedServerName(absl::string_view server_name) override {
    // Always keep the server_name_ as lower case.
    server_name_ = absl::AsciiStrToLower(server_name);
  }
  absl::string_view requestedServerName() const override { return server_name_; }

  absl::optional<std::chrono::milliseconds> lastRoundTripTime() override {
    return ioHandle().lastRoundTripTime();
  }

  void dumpState(std::ostream& os, int indent_level) const override {
    const char* spaces = spacesForLevel(indent_level);
    os << spaces << "ListenSocketImpl " << this << DUMP_MEMBER(transport_protocol_)
       << DUMP_MEMBER(server_name_) << "\n";
    DUMP_DETAILS(address_provider_);
  }

protected:
  std::string transport_protocol_;
  std::vector<std::string> application_protocols_;
  std::string server_name_;
};

// ConnectionSocket used with server connections.
class AcceptedSocketImpl : public ConnectionSocketImpl {
public:
  AcceptedSocketImpl(IoHandlePtr&& io_handle, const Address::InstanceConstSharedPtr& local_address,
                     const Address::InstanceConstSharedPtr& remote_address)
      : ConnectionSocketImpl(std::move(io_handle), local_address, remote_address) {
    ++global_accepted_socket_count_;
  }

  ~AcceptedSocketImpl() override {
    ASSERT(global_accepted_socket_count_.load() > 0);
    --global_accepted_socket_count_;
  }

  // TODO (tonya11en): Global connection count tracking is temporarily performed via a static
  // variable until the logic is moved into the overload manager.
  static uint64_t acceptedSocketCount() { return global_accepted_socket_count_.load(); }

private:
  static std::atomic<uint64_t> global_accepted_socket_count_;
};

// ConnectionSocket used with client connections.
class ClientSocketImpl : public ConnectionSocketImpl {
public:
  ClientSocketImpl(const Address::InstanceConstSharedPtr& remote_address,
                   const OptionsSharedPtr& options)
      : ConnectionSocketImpl(Network::ioHandleForAddr(Socket::Type::Stream, remote_address),
                             nullptr, remote_address) {
    if (options) {
      addOptions(options);
    }
  }
};

} // namespace Network
} // namespace Envoy
