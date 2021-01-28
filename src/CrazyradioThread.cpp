#include <iostream>

#include "CrazyradioThread.h"
#include "Crazyradio.h"
// #include "native_link/Connection.h"
#include "ConnectionImpl.h"

CrazyradioThread::CrazyradioThread(libusb_device *dev)
    : dev_(dev)
    , thread_ending_(false)
{

}

CrazyradioThread::~CrazyradioThread()
{
    const std::lock_guard<std::mutex> lock(thread_mutex_);
    if (thread_.joinable()) {
        thread_.join();
    }
}

// bool CrazyradioThread::isActive() const
// {
//     return thread_.joinable();
// }

void CrazyradioThread::addConnection(std::shared_ptr<ConnectionImpl> con)
{
    // bool startThread;
    {
        const std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.insert(con);
        // startThread = thread_ended_;
        // std::cout << "add con " << connections_.size() << std::endl;
    }

    {
        const std::lock_guard<std::mutex> lock(thread_mutex_);
        if (!thread_.joinable()) {
            // std::cout << "add con " << startThread << std::endl;
            thread_ = std::thread(&CrazyradioThread::run, this);
        }
    }

    // if (startThread) {
    //     if (thread_.joinable()) {
    //         thread_.join();
    //     }
    //     {
    //         const std::lock_guard<std::mutex> lock(connections_mutex_);
    //         thread_ended_ = false;
    //     }
    //     thread_ = std::thread(&CrazyradioThread::run, this);
    // }
}

void CrazyradioThread::removeConnection(std::shared_ptr<ConnectionImpl> con)
{
    bool endThread;
    {
        std::unique_lock<std::mutex> lk(connections_mutex_);
        connections_updated_ = false;
        connections_.erase(con);
        endThread = connections_.empty();
        connections_updated_cv_.wait(lk, [this] { return !connections_updated_; });
    }

    if (endThread) {
        const std::lock_guard<std::mutex> lock(thread_mutex_);
        thread_ending_ = true;
        thread_.join();
        thread_ = std::thread();
        thread_ending_ = false;
    }
}

void CrazyradioThread::run()
{
    Crazyradio radio(dev_);

    const uint8_t enableSafelink[] = {0xFF, 0x05, 1};
    const uint8_t ping[] = {0xFF};

    std::set<std::shared_ptr<ConnectionImpl>> connections_copy;

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // copy connections_
        bool thread_ending;
        {
            const std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_copy = connections_;
            connections_updated_ = true;
            thread_ending = thread_ending_;
        }

        connections_updated_cv_.notify_one();
        if (thread_ending)
        {
            // std::cout << "ending..." << std::endl;
            break;
        }

        for (auto con : connections_copy) {
        // for (auto con : connections_) {
            // const std::lock_guard<std::mutex> con_lock(con->alive_mutex_);
            // if (!con->alive_) {
            //     continue;
            // }
            // reconfigure radio if needed
            if (radio.address() != con->address_)
            {
                radio.setAddress(con->address_);
            }
            if (radio.channel() != con->channel_)
            {
                radio.setChannel(con->channel_);
            }
            if (radio.datarate() != con->datarate_)
            {
                radio.setDatarate(con->datarate_);
            }
            if (!radio.ackEnabled())
            {
                radio.setAckEnabled(true);
            }

            // prepare to send result
            Crazyradio::Ack ack;

            // initialize safelink if needed
            if (con->useSafelink_) {
                if (!con->safelinkInitialized_) {
                    ack = radio.sendPacket(enableSafelink, sizeof(enableSafelink));
                    ++con->statistics_.sent_count;
                    if (ack) {
                        con->safelinkInitialized_ = true;
                    }
                } else {
                    // send actual packet via safelink

                    const std::lock_guard<std::mutex> lock(con->queue_send_mutex_);
                    Packet p(ping, sizeof(ping));
                    if (!con->queue_send_.empty())
                    {
                        p = con->queue_send_.top();
                    }

                    p.setSafelink(con->safelinkUp_ << 1 | con->safelinkDown_);
                    ack = radio.sendPacket(p.raw(), p.size() + 1);
                    ++con->statistics_.sent_count;
                    if (ack && ack.size() > 0 && (ack.data()[0] & 0x04) == (con->safelinkDown_ << 2)) {
                        con->safelinkDown_ = !con->safelinkDown_;
                    }
                    if (ack)
                    {
                        con->safelinkUp_ = !con->safelinkUp_;
                        if (!con->queue_send_.empty()) {
                            con->queue_send_.pop();
                        }
                    }
                }
            } else {
                // no safelink
                const std::lock_guard<std::mutex> lock(con->queue_send_mutex_);
                if (!con->queue_send_.empty())
                {
                    const auto p = con->queue_send_.top();
                    ack = radio.sendPacket(p.raw(), p.size() + 1);
                    ++con->statistics_.sent_count;
                    if (ack)
                    {
                        con->queue_send_.pop();
                    }
                }
                else
                {
                    ack = radio.sendPacket(ping, sizeof(ping));
                    ++con->statistics_.sent_count;
                }
            }

            // enqueue result
            if (ack) {
                ++con->statistics_.ack_count;
                Packet p_ack(ack.data(), ack.size());
                if (p_ack.port() == 15 && p_ack.channel() == 3)
                {
                    // Empty packet -> update stats only
                    con->statistics_.rssi_latest = p_ack.data()[1];
                }
                else
                {
                    {
                        const std::lock_guard<std::mutex> lock(con->queue_recv_mutex_);
                        p_ack.seq_ = con->statistics_.receive_count;
                        con->queue_recv_.push(p_ack);
                        ++con->statistics_.receive_count;
                    }
                    con->queue_recv_cv_.notify_one();
                }
            }
        }
    }
}
