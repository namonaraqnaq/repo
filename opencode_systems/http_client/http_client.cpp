// http_client.cpp : This file contains the 'main' function. Program execution begins and ends there.
// and implements asynchronous http client 
//
#include <boost/exception/diagnostic_information.hpp> 
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/regex.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <string>
#include <sstream>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
using namespace std;

static void LOG(const char* what) {
	cout << "------" << what << "\n";
}

class request_buffer {
public:
	request_buffer(const char* path) :	
		path_file_(path), pbuff_(new stringstream),	pbuff_back_(new stringstream) {
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

unique_ptr<request_buffer> req_buff_ = nullptr;

static void report_failure(beast::error_code err, char const* src)
{
	std::cerr << src << ": " << err.message() << "\n";
}

// Performs an HTTP GET and prints the response
class http_session 
	: public std::enable_shared_from_this<http_session>
{
	const unsigned short keep_alive_secs_;
	const unsigned short post_req_secs_;
	tcp::resolver resolver_;
	beast::tcp_stream stream_;
	beast::flat_buffer buffer_; // (Must persist between reads)
	http::request<http::string_body> head_req_;
	http::request<http::string_body> post_req_;
	http::response<http::string_body> res_;
	
	net::io_context& ioc_;

public:
	// Objects are constructed with a strand to
	// ensure that handlers do not execute concurrently.
	explicit http_session(net::io_context& ioc, unsigned short keep_alove_secs, unsigned short post_req_secs)
		: keep_alive_secs_(keep_alove_secs)
		, post_req_secs_(post_req_secs)
		, resolver_(net::make_strand(ioc))
		, stream_(net::make_strand(ioc))
		, ioc_(ioc)
	{
		LOG("http_session constructor");
	}

	// Start the asynchronous operation
	void run(char const* host, char const* port)
	{
		LOG("http_session run");
		// Set up an HTTP GET request message
		std::string const payload("keep_alive");
		post_req_.content_length(payload.size());
		post_req_.body() = payload;

		head_req_.version(11);
		head_req_.method(http::verb::head);
		head_req_.set(http::field::host, host);
		head_req_.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
		head_req_.keep_alive(true);
		
		post_req_.version(11);
		post_req_.method(http::verb::post);
		post_req_.set(http::field::host, host);
		post_req_.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
		post_req_.keep_alive(true);

		// Look up the domain name
		resolver_.async_resolve(host, port,
				beast::bind_front_handler(&http_session::on_resolve, shared_from_this()));
	}

	void on_resolve(beast::error_code err, tcp::resolver::results_type results)
	{
		LOG("http_session on_resolve");
		if (err) {
			return report_failure(err, "resolve");
		}
		// Set a timeout on the operation
		stream_.expires_after(std::chrono::seconds(30));

		// Make the connection on the IP address we get from a lookup
		stream_.async_connect(results,
				beast::bind_front_handler(&http_session::on_connect, shared_from_this()));
	}

	void do_head_request() {
		LOG("http_session do_head_request");
		net::deadline_timer tmr(ioc_, boost::posix_time::seconds(15));
		tmr.async_wait(beast::bind_front_handler(&http_session::head_request, shared_from_this()));
		//tmr.async_wait(boost::bind(&http_session::post_request, this, boost::asio::placeholders::error));
	}

	void head_request(beast::error_code err) {
		LOG("http_session head_request");
		if (err) {
			report_failure(err, "head request");
			// Close the socket
			stream_.socket().shutdown(tcp::socket::shutdown_both, err);
			// not_connected happens sometimes so don't bother reporting it.
			if (err && err != beast::errc::not_connected)
				return report_failure(err, "shutdown");
			// If we get here then the connection is closed gracefully
			return;
		}
		// Set a timeout on the operation
		stream_.expires_after(std::chrono::seconds(30));

		// Send the HTTP request to the remote host	
		http::async_write(stream_, head_req_,
			beast::bind_front_handler(&http_session::on_write, shared_from_this()));

		do_head_request();
	}

	void do_post_request() {
		LOG("http_session do_post_request");
		net::deadline_timer tmr(ioc_, boost::posix_time::seconds(5));
		tmr.async_wait(beast::bind_front_handler(&http_session::post_request, shared_from_this()));
		//tmr.async_wait(boost::bind(&http_session::post_request, this, boost::asio::placeholders::error));
	}

	void post_request(beast::error_code err) {
		LOG("http_session post_request");
		if (err) {
			report_failure(err, "post request");
			// Close the socket
			stream_.socket().shutdown(tcp::socket::shutdown_both, err);
			// not_connected happens sometimes so don't bother reporting it.
			if (err && err != beast::errc::not_connected)
				return report_failure(err, "shutdown");
			// If we get here then the connection is closed gracefully
			return;
		}
		// Set a timeout on the operation
		stream_.expires_after(std::chrono::seconds(30));

		std::string const payload(req_buff_->str());
		post_req_.content_length(payload.size());
		post_req_.body() = payload;

		// Send the HTTP request to the remote host
		http::async_write(stream_, post_req_,
			beast::bind_front_handler(&http_session::on_write, shared_from_this()));
		
		do_post_request();
	}

	void on_connect(beast::error_code err, tcp::resolver::results_type::endpoint_type)
	{
		LOG("http_session on_connect");
		if (err) {
			return report_failure(err, "connect");
		}
		head_request(err);
		post_request(err);
	}

	void on_write(beast::error_code err, std::size_t bytes_transferred)
	{
		LOG("http_session on_write");
		boost::ignore_unused(bytes_transferred);
		if (err) {
			return report_failure(err, "write");
		}
		// Receive the HTTP response
		http::async_read(stream_, buffer_, res_,
			beast::bind_front_handler(&http_session::on_read, shared_from_this()));
	}

	void on_read(beast::error_code err, std::size_t bytes_transferred)
	{
		LOG("http_session on_read");
		boost::ignore_unused(bytes_transferred);
		if (err) {
			return report_failure(err, "read");
		}
		// Write the message to standard out
		std::cout << res_ << std::endl;		
	}
};

static int input_params_error() {
	cerr <<
		"Usage: http_client --host=hostname --port=portnumber --keep-alive=seconds --post-request=seconds --reload=seconds --request=path_to_request.json" <<
		"Example:\n" <<
		"    http_client --host=www.example.com --port=80 --keep-alive=10 --post-request=20 --reload=30 --request=request.json";
	return EXIT_FAILURE;
}

static void update_req_buffer(net::io_context& ioc, unsigned short reload_secs)
{
	LOG("update_req_buffer on_read");
	do {
		std::this_thread::sleep_for(std::chrono::milliseconds(reload_secs * 1000));
		req_buff_->reload();
	} while (!ioc.stopped());
}

int main(int argc, char *argv[])
{
	if (argc != 7) {
		return input_params_error();		
	}
	map<string, string> settings;
	boost::regex args_format("^--(.+)=(.+)$");
	boost::cmatch cm;
	for (int i = 1; i < argc; ++i) {		
		if (!boost::regex_match(argv[i], cm, args_format)){
			return input_params_error();
		}
		settings[cm[1].str()] = cm[2].str();
	}

	const auto host(settings["host"]);
	const auto port(settings["port"]);
	if (host.empty() || port.empty()) {
		return input_params_error();
	}

	const auto keep_alive_secs = atoi(settings["keep-alive"].c_str());
	const auto post_request_secs = atoi(settings["post-request"].c_str());
	const auto reload_secs = atoi(settings["reload"].c_str());
	if (keep_alive_secs <= 0 || post_request_secs <= 0 || reload_secs <= 0) {
		return input_params_error();
	}

	req_buff_ = make_unique<request_buffer>(settings["request"].c_str());
	if (!req_buff_->good()) {
		cerr << "bad path_to_request" << endl;
		return input_params_error();
	}

	// The io_context is required for all I/O
	try {
	net::io_context ioc;
	thread tu(update_req_buffer, std::ref(ioc), reload_secs);
	// Launch the asynchronous operation
	std::make_shared<http_session>(ioc, keep_alive_secs, post_request_secs)->run(host.c_str(), port.c_str());
	// Run the I/O service. The call will return when
	// the get operation is complete.
	ioc.run();
	} catch (std::exception const&  ex) {
		cerr << ex.what();
	} catch (const boost::exception& be) {
		cerr << diagnostic_information(be);
	} catch (...) {
		LOG("BOOM");
	}
	return EXIT_SUCCESS;
}

