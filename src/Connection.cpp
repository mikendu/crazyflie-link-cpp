#include <regex>
#include <iostream>
#include <future>

#include "native_link/Connection.h"
#include "ConnectionImpl.h"

#include "USBManager.h"
#include "Crazyradio.h"

#include <libusb-1.0/libusb.h>

Connection::Connection(const std::string &uri, const Connection::Settings &settings)
    : impl_(std::make_shared<ConnectionImpl>())
{
  // Examples:
  // "usb://0" -> connect over USB
  // "radio://0/80/2M/E7E7E7E7E7" -> connect over radio
  // "radio://*/80/2M/E7E7E7E7E7" -> auto-pick radio
  // "radio://*/80/2M/*" -> broadcast/P2P sniffing on channel 80

  const std::regex uri_regex("(usb:\\/\\/(\\d+)|radio:\\/\\/(\\d+|\\*)\\/(\\d+)\\/(250K|1M|2M)\\/([a-fA-F0-9]{10}|\\*))");
  std::smatch match;
  if (!std::regex_match(uri, match, uri_regex)) {
    throw std::runtime_error("Invalid uri!");
  }

  impl_->uri_ = uri;

  // for (size_t i = 0; i < match.size(); ++i) {
  //   std::cout << i << " " << match[i].str() << std::endl;
  // }
  // std::cout << match.size() << std::endl;

  if (match[2].matched) {
    // usb://
    impl_->devid_ = std::stoi(match[2].str());
    auto dev = USBManager::get().crazyfliesOverUSB().at(impl_->devid_);
    impl_->crazyflieUSB_ = std::make_shared<CrazyflieUSB>(dev);
  } else {
    // radio
    if (match[3].str() == "*") {
      impl_->devid_ = -1;
    } else {
      impl_->devid_ = std::stoi(match[3].str());
    }

    impl_->channel_ = std::stoi(match[4].str());
    if (match[5].str() == "250K") {
      impl_->datarate_ = Crazyradio::Datarate_250KPS;
    } else if (match[5].str() == "1M") {
      impl_->datarate_ = Crazyradio::Datarate_1MPS;
    } else if (match[5].str() == "2M") {
      impl_->datarate_ = Crazyradio::Datarate_2MPS;
    }
    impl_->address_ = std::stoul(match[6].str(), nullptr, 16);
    impl_->useSafelink_ = settings.use_safelink;
    impl_->safelinkInitialized_ = false;
    impl_->safelinkUp_ = false;
    impl_->safelinkDown_ = false;

    USBManager::get().addRadioConnection(impl_);
  }

}

Connection::~Connection()
{
  // std::cout << "~Connection " << impl_->uri_ << std::endl;
  if (!impl_->crazyflieUSB_) {
    USBManager::get().removeRadioConnection(impl_);
  }
}

std::vector<std::string> Connection::scan(const std::string& address)
{
  std::vector<std::string> result;

  // Crazyflies over USB
  for (size_t i = 0; i < USBManager::get().numCrazyfliesOverUSB(); ++i) {
    result.push_back("usb://" + std::to_string(i));
  }

  // Crazyflies over radio
  std::string a = address;
  if (address.empty()) {
    a = "E7E7E7E7E7";
  }

  std::vector<std::future<std::string>> futures;
  for (auto datarate : {"250K", "1M", "2M"})
  {
    for (int channel = 0; channel < 125; ++channel) {
      std::string uri = "radio://*/" + std::to_string(channel) + "/" + datarate + "/" + a;

      futures.emplace_back(std::async(std::launch::async,
      [uri]() {
        Connection con(uri);
        bool success;
        while (true)
        {
          if (con.statistics().sent_count >= 1)
          {
            success = con.statistics().ack_count >= 1;
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        if (success) {
          return uri;
        }
        return std::string();
      }));
    }
  }

  for (auto& future : futures) {
    auto uri = future.get();
    if (!uri.empty())
    {
      result.push_back(uri);
    }
  }

  return result;
}

void Connection::send(const Packet& p)
{
  if (!impl_->crazyflieUSB_) {
    impl_->crazyflieUSB_->send(p.raw(), p.size()+1);
  }
  else {
    const std::lock_guard<std::mutex> lock(impl_->queue_send_mutex_);
    p.seq_ = impl_->statistics_.enqueued_count;
    impl_->queue_send_.push(p);
    ++impl_->statistics_.enqueued_count;
  }
}

Packet Connection::recv(bool blocking)
{
  if (!impl_->crazyflieUSB_) {
    Packet result;
    size_t size = impl_->crazyflieUSB_->recv(result.data(), CRTP_MAXSIZE, blocking ? 0 : 100);
    result.setSize(size);
    return result;
  } else {
    if (blocking) {
      std::unique_lock<std::mutex> lk(impl_->queue_recv_mutex_);
      impl_->queue_recv_cv_.wait(lk, [this] { return !impl_->queue_recv_.empty(); });
      auto result = impl_->queue_recv_.top();
      impl_->queue_recv_.pop();
      return result;
    } else {
      const std::lock_guard<std::mutex> lock(impl_->queue_recv_mutex_);

      Packet result;
      if (impl_->queue_recv_.empty()) {
        return result;
      } else {
        result = impl_->queue_recv_.top();
        impl_->queue_recv_.pop();
      }
      return result;
    }
  }
}

std::ostream& operator<<(std::ostream& out, const Connection& p)
{
  out <<"Connection(" << p.impl_->uri_;
  out << ")";
  return out;
}

const std::string& Connection::uri() const
{
  return impl_->uri_;
}

Connection::Statistics& Connection::statistics()
{
  return impl_->statistics_;
}