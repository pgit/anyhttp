#include <boost/asio.hpp>
#include <stdexec/execution.hpp>
#include <iostream>
#include <vector>
#include <string_view>

namespace asio = boost::asio;
namespace ex = stdexec;

class async_read_until_sender {
public:
    async_read_until_sender(asio::ip::tcp::socket& socket, std::vector<char>& buffer, char delimiter)
        : socket_(socket), buffer_(buffer), delimiter_(delimiter) {}

    template <typename Receiver>
    struct operation {
        asio::ip::tcp::socket& socket_;
        std::vector<char>& buffer_;
        char delimiter_;
        Receiver receiver_;

        void start() {
            do_read();
        }

    private:
        void do_read() {
            buffer_.resize(buffer_.size() + 512);
            socket_.async_read_some(asio::buffer(buffer_.data() + buffer_.size() - 512, 512),
                [this](boost::system::error_code ec, std::size_t bytes_transferred) mutable {
                    if (ec) {
                        ex::set_error(std::move(receiver_), ec);
                        return;
                    }

                    buffer_.resize(buffer_.size() - 512 + bytes_transferred);
                    auto pos = std::find(buffer_.begin(), buffer_.end(), delimiter_);
                    if (pos != buffer_.end()) {
                        std::size_t delimiter_pos = std::distance(buffer_.begin(), pos);
                        buffer_.resize(delimiter_pos + 1);
                        ex::set_value(std::move(receiver_), delimiter_pos + 1);
                    } else {
                        do_read();
                    }
                }
            );
        }
    };

    template <typename Receiver>
    operation<Receiver> connect(Receiver receiver) {
        return {socket_, buffer_, delimiter_, std::move(receiver)};
    }

private:
    asio::ip::tcp::socket& socket_;
    std::vector<char>& buffer_;
    char delimiter_;
};