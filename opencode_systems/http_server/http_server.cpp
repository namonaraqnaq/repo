// http_server.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
//#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time.hpp>
#include <boost/regex.hpp> 
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/config.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <queue>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
using namespace std;

class response_buffer {
public:
	response_buffer(const char* path) :
		path_file_(path), pbuff_(new stringstream), pbuff_back_(new stringstream) {
		reload();
	}

	const char* str() const { return pbuff_->str().c_str(); }
	bool good() const { return good_; }
	bool reload() {
		ifstream t(path_file_);
		if (good_ = t.good()) {
			pbuff_back_->str("");
			*pbuff_back_ << t.rdbuf();
			swap(pbuff_, pbuff_back_);
		}
		return good_;
	}

private:
	bool good_;
	const char* path_file_;
	unique_ptr<stringstream> pbuff_;
	unique_ptr<stringstream> pbuff_back_;
};

unique_ptr<response_buffer> resp_buff_ = nullptr;

struct request_data {
	boost::posix_time::ptime timestamp_;
	string server_port_;
	string client_port_;	
	string data_;
};

class request_data_q {
	std::queue<request_data> queue_;
	mutable std::mutex mutex_;
public:
	request_data_q() = default;
	request_data_q(const request_data_q&) = delete;
	request_data_q& operator=(const request_data_q&) = delete;
	~request_data_q() {}

	void pop() {
		std::lock_guard<std::mutex> lock(mutex_);
		if (queue_.empty()) {
			return;
		}
		request_data dt = queue_.front();
		string tms = to_iso_string(dt.timestamp_);
		tms = tms.substr(0, tms.find("."));
		string filename =	dt.server_port_ + 
			"_" + dt.client_port_ + "_" + 
			tms + ".txt";

		ofstream outfile(filename.c_str());
		outfile << dt.data_;
		queue_.pop();
	}

	void push(const request_data &item) {
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push(item);
	}
} request_data_q_;

// Append an HTTP rel-path to a local filesystem path.
// The returned path is normalized for the platform.
std::string path_cat(beast::string_view base, beast::string_view path)
{
	if (base.empty()) {
		return std::string(path);
	}
	std::string result(base);
#ifdef BOOST_MSVC
	char constexpr path_separator = '\\';
	if (result.back() == path_separator)
		result.resize(result.size() - 1);
	result.append(path.data(), path.size());
	for (auto& c : result)
		if (c == '/')
			c = path_separator;
#else
	char constexpr path_separator = '/';
	if (result.back() == path_separator)
		result.resize(result.size() - 1);
	result.append(path.data(), path.size());
#endif
	return result;
}

// Return a response for the given request.
template <class Body, class Allocator>
http::message_generator handle_request(	beast::string_view doc_root,
										http::request<Body, http::basic_fields<Allocator>>&& req)
{	
	// Returns a bad request response
	auto const bad_request = [&req](beast::string_view why) {
		http::response<http::string_body> res{ http::status::bad_request, req.version() };
		res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
		res.set(http::field::content_type, "text/html");
		res.keep_alive(req.keep_alive());
		res.body() = std::string(why);
		res.prepare_payload();
		return res;
	};	
	// Make sure we can handle the method
	if (req.method() != http::verb::post &&
		req.method() != http::verb::head)
		return bad_request("Unknown HTTP-method");
	// Request path must be absolute and not contain "..".
	if (req.target().empty() ||
		req.target()[0] != '/' ||
		req.target().find("..") != beast::string_view::npos)
		return bad_request("Illegal request-target");
	// Respond to HEAD request
	if (req.method() == http::verb::head) {
		http::response<http::empty_body> res{ http::status::ok, req.version() };
		res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
		res.set(http::field::content_type, "text/html");
		res.content_length(0);
		res.keep_alive(req.keep_alive());
		return res;
	}
	// Respond to GET request
	static std::string const payload(resp_buff_->str());
	http::response<http::string_body> res; 
	res.result(http::status::ok);
	res.version(11);
	res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
	res.set(http::field::content_type, "text/html");	
	res.content_length(payload.size());
	res.body() = payload;	
	res.keep_alive(req.keep_alive());
	return res;
}

static void report_failure(beast::error_code err, char const* src)
{
	std::cerr << src << ": " << err.message() << "\n";
}

// Handles an HTTP server connection
class http_session : public std::enable_shared_from_this<http_session>
{
	beast::tcp_stream stream_;
	beast::flat_buffer buffer_;
	std::shared_ptr<std::string const> doc_root_;
	http::request<http::string_body> req_;
	request_data rd_;

public:
	// Take ownership of the stream
	http_session(tcp::socket&& socket, std::shared_ptr<std::string const> const& doc_root)
		: stream_(std::move(socket))
		, doc_root_(doc_root)
	{
		rd_.server_port_ = std::to_string(socket.local_endpoint().port());
		rd_.client_port_ = std::to_string(socket.remote_endpoint().port());
	}
	// Start the asynchronous operation
	void run()
	{
		// We need to be executing within a strand to perform async operations
		// on the I/O objects in this session. Although not strictly necessary
		// for single-threaded contexts, this example code is written to be
		// thread-safe by default.
		net::dispatch(	stream_.get_executor(), 
						beast::bind_front_handler(&http_session::do_read, shared_from_this()));
	}

	void do_read()
	{
		// Make the request empty before reading, otherwise the operation behavior is undefined.
		req_ = {};
		// Set the timeout.
		stream_.expires_after(std::chrono::seconds(30));
		// Read a request
		http::async_read(stream_, buffer_, req_,
			beast::bind_front_handler(&http_session::on_read, shared_from_this()));
	}

	void send_response(http::message_generator&& msg)
	{
		bool keep_alive = msg.keep_alive();
		// Write the response
		beast::async_write(	stream_,
							std::move(msg),
							beast::bind_front_handler(&http_session::on_write, shared_from_this(), keep_alive));
	}

	void on_read(beast::error_code ec, std::size_t bytes_transferred)
	{
		boost::ignore_unused(bytes_transferred);
		// This means they closed the connection
		if (ec == http::error::end_of_stream)
			return do_close();
		if (ec)
			return report_failure(ec, "read");
		
		// Write request
		rd_.timestamp_ = boost::posix_time::second_clock::local_time();
		rd_.data_ = req_.body();
		request_data_q_.push(rd_);

		// Send the response
		send_response(handle_request(*doc_root_, std::move(req_)));
	}	

	void on_write(bool keep_alive, beast::error_code ec, std::size_t bytes_transferred)
	{
		boost::ignore_unused(bytes_transferred);
		if (ec)
			return report_failure(ec, "write");
		if (!keep_alive) {
			// This means we should close the connection, usually because
			// the response indicated the "Connection: close" semantic.
			return do_close();
		}
		// Read another request
		do_read();
	}

	void do_close()
	{
		// Send a TCP shutdown
		beast::error_code ec;
		stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
		// At this point the connection is closed gracefully
	}
};

// Accepts incoming connections and launches the sessions
class listener : public std::enable_shared_from_this<listener>
{
	net::io_context& ioc_;
	tcp::acceptor acceptor_;
	shared_ptr<string const> doc_root_;
	
public:
	listener(	net::io_context& ioc, 
				tcp::endpoint endpoint, 
				shared_ptr<string const> const& doc_root)
		: ioc_(ioc)
		, acceptor_(net::make_strand(ioc))
		, doc_root_(doc_root)
	{
		beast::error_code ec;
		// Open the acceptor
		acceptor_.open(endpoint.protocol(), ec);
		if (ec) {
			report_failure(ec, "open");
			return;
		}
		// Allow address reuse
		acceptor_.set_option(net::socket_base::reuse_address(true), ec);
		if (ec) {
			report_failure(ec, "set_option");
			return;
		}
		// Bind to the server address
		acceptor_.bind(endpoint, ec);
		if (ec) {
			report_failure(ec, "bind");
			return;
		}
		// Start listening for connections
		acceptor_.listen( net::socket_base::max_listen_connections, ec);
		if (ec)	{
			report_failure(ec, "listen");
			return;
		}
	}

	// Start accepting incoming connections
	void run()
	{
		do_accept();
	}

private:
	void do_accept()
	{
		// The new connection gets its own strand
		acceptor_.async_accept(	net::make_strand(ioc_),
								beast::bind_front_handler(&listener::on_accept, shared_from_this()));
	}

	void on_accept(beast::error_code ec, tcp::socket socket)
	{
		if (ec) {
			report_failure(ec, "accept");
			return; // To avoid infinite loop
		} 
		// Create the session and run it
		std::make_shared<http_session>(std::move(socket), doc_root_)->run();
		// Accept another connection
		do_accept();
	}
};

static int input_params_error() {
	cerr <<
		"Usage: --port=portnumber --reload=seconds --response=path_to_response.json" <<
		"Example:\n" <<
		"    http_server --port=80 --reload=30 --response=response.json";
	return EXIT_FAILURE;
}

static void update_resp_buffer(net::io_context& ioc, unsigned short reload_secs)
{
	do {
		std::this_thread::sleep_for(std::chrono::milliseconds(reload_secs*1000));
	} while (!ioc.stopped());
}

static void write_responses(net::io_context& ioc) {
	do {
		request_data_q_.pop();
	} while (!ioc.stopped());
}

int main(int argc, char *argv[])
{
	if (argc != 4) {
		return input_params_error();
	}
	map<string, string> settings;
	boost::regex args_format("^--(.+)=(.+)$");
	boost::cmatch cm;
	for (int i = 1; i < argc; ++i) {
		if (!boost::regex_match(argv[i], cm, args_format)) {
			return input_params_error();
		}
		settings[cm[1].str()] = cm[2].str();
	}
	
	if (settings["port"].empty()) {
		return input_params_error();
	}	
	const auto port = static_cast<unsigned short>(atoi(settings["port"].c_str()));
	auto const address = net::ip::make_address("127.0.0.1");
	auto const doc_root = std::make_shared<std::string>(".");

	const auto reload_secs = atoi(settings["reload"].c_str());
	if (reload_secs <= 0) {
		return input_params_error();
	}
	resp_buff_ = make_unique<response_buffer>(settings["response"].c_str());
	if (!resp_buff_->good()) {
		cerr << "bad path_to_response" << endl;
		return input_params_error();
	}
	
	net::io_context ioc;
	// Create and launch a listening port
	std::make_shared<listener>(
		ioc,
		tcp::endpoint{ address, port },
		doc_root)->run();
	ioc.run();

	thread(write_responses, std::ref(ioc)).join();
	thread(update_resp_buffer, std::ref(ioc), reload_secs).join();

	return EXIT_SUCCESS;
}

