#ifndef OPENTICK_CONNECTION_H_
#define OPENTICK_CONNECTION_H_

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include "boost/endian/conversion.hpp"

#include "json.hpp"

namespace opentick {

using json = nlohmann::json;

typedef std::chrono::system_clock::time_point Tm;
typedef std::variant<std::int64_t, std::uint64_t, std::int32_t, std::uint32_t,
                     bool, float, double, std::nullptr_t, std::string, Tm>
    ValueScalar;
typedef std::vector<std::vector<ValueScalar>> ValuesVector;
typedef std::shared_ptr<ValuesVector> ResultSet;
typedef std::variant<ResultSet, ValueScalar> Value;
struct AbstractFuture {
  virtual ResultSet Get(double timeout = 0) = 0;  // timeout in seconds
};
typedef std::shared_ptr<AbstractFuture> Future;
typedef std::vector<ValueScalar> Args;
typedef std::vector<Args> Argss;

class Connection : public std::enable_shared_from_this<Connection> {
 public:
  typedef std::shared_ptr<Connection> Ptr;
  bool IsConnected() const;
  void Use(const std::string& dbName);
  Future ExecuteAsync(const std::string& sql, const Args& args = Args{});
  ResultSet Execute(const std::string& sql, const Args& args = Args{});
  Future BatchInsertAsync(const std::string& sql, const Argss& argss);
  void BatchInsert(const std::string& sql, const Argss& argss);
  int Prepare(const std::string& sql);
  void Close();

 protected:
  Connection(const std::string& addr, int port);
  void ReadHead();
  void ReadBody(unsigned len);
  template <typename T>
  void Send(T&& msg);
  void Write();
  void Notify(int, const Value&);

 private:
  std::vector<std::uint8_t> msg_in_buf_;
  std::vector<std::uint8_t> msg_out_buf_;
  std::vector<std::uint8_t> outbox_;
  boost::asio::io_service io_service_;
  boost::asio::io_service::work worker_;
  boost::asio::ip::tcp::socket socket_;
  std::thread thread_;
  std::atomic<int> ticker_counter_ = 0;
  std::condition_variable cv_;
  std::mutex m_cv_;
  std::mutex m_;
  std::map<std::string, int> prepared_;
  std::map<int, Value> store_;
  friend class FutureImpl;
  friend Ptr Connect(const std::string&, int, const std::string&);
};

inline Connection::Ptr Connect(const std::string& addr, int port,
                               const std::string& db_name = "") {
  Connection::Ptr conn(new Connection(addr, port));
  conn->ReadHead();
  if (db_name.size()) {
    conn->Use(db_name);
  }
  return conn;
}

class Exception : public std::exception {
 public:
  Exception(const std::string& m) : m_(m) {}
  const char* what() const noexcept override { return m_.c_str(); }

 private:
  std::string m_;
};

struct FutureImpl : public AbstractFuture {
  ResultSet Get(double timeout = 0) override;
  Value Get_(double timeout = 0);
  FutureImpl(int t, Connection::Ptr c) : ticker(t), conn(c) {}
  int ticker;
  Connection::Ptr conn;
};

inline Connection::Connection(const std::string& ip, int port)
    : worker_(io_service_),
      socket_(io_service_),
      thread_([this]() { io_service_.run(); }) {
  try {
    boost::asio::ip::tcp::endpoint end_pt(
        boost::asio::ip::address::from_string(ip), port);
    std::cerr << "OpenTick: connecting ..." << std::endl;
    socket_.connect(end_pt);
    boost::asio::ip::tcp::no_delay option(true);
    socket_.set_option(option);
  } catch (std::exception& e) {
    std::cerr << "OpenTick: failed to connect: " << e.what() << std::endl;
  }
}

inline bool Connection::IsConnected() const { return socket_.is_open(); }

inline void Connection::Close() {
  auto self = shared_from_this();
  io_service_.post([self]() {
    boost::system::error_code ignoredCode;
    try {
      self->socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                             ignoredCode);
      self->socket_.close();
    } catch (...) {
    }
  });
}

inline void Connection::Use(const std::string& dbName) {
  auto ticker = ++ticker_counter_;
  Send(json::to_bson(json{{"0", ticker}, {"1", "use"}, {"2", dbName}}));
  FutureImpl(ticker, shared_from_this()).Get();
}

inline void Connection::ReadHead() {
  if (msg_in_buf_.size() < 4) msg_in_buf_.resize(4);
  auto self = shared_from_this();
  boost::asio::async_read(socket_, boost::asio::buffer(msg_in_buf_, 4),
                          [=](const boost::system::error_code& e, size_t) {
                            if (e) {
                              std::cerr << "OpenTick: connection closed: "
                                        << e.message() << std::endl;
                              self->Notify(-1, e.message());
                              return;
                            }
                            unsigned n;
                            memcpy(&n, msg_in_buf_.data(), 4);
                            n = boost::endian::little_to_native(n);
                            if (n)
                              self->ReadBody(n);
                            else
                              self->ReadHead();
                          });
}

inline void Connection::ReadBody(unsigned len) {
  msg_in_buf_.resize(len);
  auto self = shared_from_this();
  boost::asio::async_read(
      socket_, boost::asio::buffer(msg_in_buf_, len),
      [self, len](const boost::system::error_code& e, size_t) {
        if (e) {
          std::cerr << "OpenTick: connection closed" << std::endl;
          self->Notify(-1, e.message());
          return;
        }
        if (len == 1 && self->msg_in_buf_[0] == 'H') {
          self->Send(std::string(""));
          self->ReadHead();
          return;
        }
        try {
          auto j = json::from_bson(self->msg_in_buf_);
          auto ticker = j["0"].get<std::int64_t>();
          auto tmp = j["1"];
          if (tmp.is_string()) {
            self->Notify(ticker, tmp.get<std::string>());
          } else if (tmp.is_number_integer()) {
            self->Notify(ticker, tmp.get<std::int64_t>());
          } else if (tmp.is_number_float()) {
            self->Notify(ticker, tmp.get<double>());
          } else if (tmp.is_boolean()) {
            self->Notify(ticker, tmp.get<bool>());
          } else if (tmp.is_null()) {
            self->Notify(ticker, ValueScalar(nullptr));
          } else {
            auto v = std::make_shared<ValuesVector>();
            v->resize(tmp.size());
            for (auto i = 0u; i < tmp.size(); ++i) {
              auto& tmp2 = tmp[i];
              auto& v2 = (*v)[i];
              v2.resize(tmp2.size());
              for (auto j = 0u; j < tmp2.size(); ++j) {
                auto& v3 = v2[j];
                auto tmp3 = tmp2[j];
                if (tmp3.is_string()) {
                  v3 = tmp3.get<std::string>();
                } else if (tmp3.is_number_integer()) {
                  v3 = tmp3.get<std::int64_t>();
                } else if (tmp3.is_number_float()) {
                  v3 = tmp3.get<double>();
                } else if (tmp3.is_boolean()) {
                  v3 = tmp3.get<bool>();
                } else if (tmp3.is_array() && tmp3.size() == 2) {
                  auto sec = tmp3[0].get<std::int64_t>();
                  auto nsec = tmp3[1].get<std::int64_t>();
                  v3 = std::chrono::system_clock::from_time_t(sec) +
                       std::chrono::nanoseconds(nsec);
                } else {
                  v3 = nullptr;
                }
              }
            }
            self->Notify(ticker, v);
          }
        } catch (nlohmann::detail::parse_error& e) {
          std::cerr << "OpenTick: invalid bson" << std::endl;
        } catch (nlohmann::detail::exception& e) {
          std::cerr << "OpenTick: bson error: " << e.what() << std::endl;
        }
        self->ReadHead();
      });
}

template <typename T>
inline void Connection::Send(T&& msg) {
  if (!IsConnected()) return;
  auto self = shared_from_this();
  io_service_.post([msg = std::move(msg), self]() {
    auto& buf = self->msg_out_buf_;
    auto n0 = buf.size();
    buf.resize(n0 + 4 + msg.size());
    unsigned n = boost::endian::native_to_little(msg.size());
    memcpy(reinterpret_cast<void*>(buf.data() + n0), &n, 4);
    memcpy(reinterpret_cast<void*>(buf.data() + n0 + 4), msg.data(),
           msg.size());
    if (self->outbox_.size()) return;
    self->Write();
  });
}

inline void Connection::Write() {
  assert(outbox_.empty());
  outbox_.swap(msg_out_buf_);
  auto self = shared_from_this();
  boost::asio::async_write(
      socket_, boost::asio::buffer(outbox_, outbox_.size()),
      [self](const boost::system::error_code& e, std::size_t) {
        if (e) {
          std::cerr << "OpenTick: failed to send message. Error code: "
                    << e.message() << std::endl;
          self->Notify(-1, e.message());
        } else {
          self->outbox_.clear();
          if (self->msg_out_buf_.size()) self->Write();
        }
      });
}

inline int Connection::Prepare(const std::string& sql) {
  {
    std::lock_guard<std::mutex> lock(m_);
    auto it = prepared_.find(sql);
    if (it != prepared_.end()) return it->second;
  }
  auto ticker = ++ticker_counter_;
  Send(json::to_bson(json{{"0", ticker}, {"1", "prepare"}, {"2", sql}}));
  FutureImpl f(ticker, shared_from_this());
  auto id = std::get<std::int64_t>(std::get<ValueScalar>(f.Get_()));
  {
    std::lock_guard<std::mutex> lock(m_);
    prepared_.emplace(sql, id);
  }
  return id;
}

inline Value FutureImpl::Get_(double timeout) {
  std::unique_lock<std::mutex> lk(conn->m_cv_);
  auto start = std::chrono::system_clock::now();
  while (true) {
    auto it1 = conn->store_.find(ticker);
    if (it1 != conn->store_.end()) {
      if (auto ptr = std::get_if<ValueScalar>(&it1->second)) {
        if (auto ptr2 = std::get_if<std::string>(ptr)) throw Exception(*ptr2);
      }
      return it1->second;
    }
    auto it2 = conn->store_.find(-1);
    if (it2 != conn->store_.end()) {
      throw Exception(
          std::get<std::string>(std::get<ValueScalar>(it2->second)));
    }
    using namespace std::chrono_literals;
    conn->cv_.wait_for(lk, 1ms);
    if (timeout > 0 &&
        (std::chrono::system_clock::now() - start).count() >= timeout) {
      throw Exception("Timeout");
    }
  }
  return {};
}

inline ResultSet FutureImpl::Get(double timeout) {
  auto v = Get_(timeout);
  if (auto ptr = std::get_if<ResultSet>(&v)) return *ptr;
  return {};
}

inline void Connection::Notify(int ticker, const Value& value) {
  std::lock_guard<std::mutex> lk(m_cv_);
  store_[ticker] = value;
  cv_.notify_all();
}

void ConvertArgs(const Args& args, json& jargs) {
  for (auto& v : args) {
    std::visit(
        [&jargs](auto&& v2) {
          using T = std::decay_t<decltype(v2)>;
          if constexpr (std::is_same_v<T, Tm>) {
            auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         v2.time_since_epoch())
                         .count();
            jargs.push_back(json{d / 1000000000, d % 1000000000});
          } else {
            jargs.push_back(v2);
          }
        },
        v);
  }
}

inline Future Connection::ExecuteAsync(const std::string& sql,
                                       const Args& args) {
  auto prepared = -1;
  json jargs;
  if (args.size()) {
    ConvertArgs(args, jargs);
    prepared = Prepare(sql);
  }
  auto ticker = ++ticker_counter_;
  json j = {{"0", ticker}, {"1", "run"}, {"2", sql}, {"3", jargs}};
  if (prepared >= 0) j["2"] = prepared;
  Send(json::to_bson(j));
  return Future(new FutureImpl(ticker, shared_from_this()));
}

inline ResultSet Connection::Execute(const std::string& sql, const Args& args) {
  return ExecuteAsync(sql, args)->Get();
}

inline Future Connection::BatchInsertAsync(const std::string& sql,
                                           const Argss& argss) {
  std::vector<json> data;
  data.resize(argss.size());
  auto i = 0u;
  for (auto& args : argss) {
    ConvertArgs(args, data[i++]);
  }
  auto prepared = Prepare(sql);
  auto ticker = ++ticker_counter_;
  Send(json::to_bson(
      json{{"0", ticker}, {"1", "batch"}, {"2", prepared}, {"3", data}}));
  return Future(new FutureImpl(ticker, shared_from_this()));
}

inline void Connection::BatchInsert(const std::string& sql,
                                    const Argss& argss) {
  BatchInsertAsync(sql, argss)->Get();
}

}  // namespace opentick

#endif  // OPENTICK_CONNECTION_H_
