// http_client.cpp : This file contains the 'main' function. Program execution begins and ends there.
// and implements asynchronous http client 
//
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

static void report_failure(beast::error_code err, char const* src)
{
	std::cerr << src << ": " << err.message() << "\n";
}

// Performs an HTTP GET and prints the response
class http_session 
	: public std::enable_shared_from_this<http_session>
{
	tcp::resolver resolver_;
	beast::tcp_stream stream_;
	beast::flat_buffer buffer_; // (Must persist between reads)
	http::request<http::empty_body> req_;
	http::response<http::string_body> res_;

public:
	// Objects are constructed with a strand to
	// ensure that handlers do not execute concurrently.
	explicit http_session(net::io_context& ioc)
		: resolver_(net::make_strand(ioc))
		, stream_(net::make_strand(ioc))
	{
	}

	// Start the asynchronous operation
	void run(char const* host, char const* port, char const* target)
	{
		// Set up an HTTP GET request message
		req_.method(http::verb::get);
		req_.target(target);
		req_.set(http::field::host, host);
		req_.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

		// Look up the domain name
		resolver_.async_resolve(host, port,
				beast::bind_front_handler(&http_session::on_resolve, shared_from_this()));
	}

	void on_resolve(beast::error_code err, tcp::resolver::results_type results)
	{
		if (err) {
			return report_failure(err, "resolve");
		}
		// Set a timeout on the operation
		stream_.expires_after(std::chrono::seconds(30));

		// Make the connection on the IP address we get from a lookup
		stream_.async_connect(results,
				beast::bind_front_handler(&http_session::on_connect, shared_from_this()));
	}

	void on_connect(beast::error_code err, tcp::resolver::results_type::endpoint_type)
	{
		if (err) {
			return report_failure(err, "connect");
		}
		// Set a timeout on the operation
		stream_.expires_after(std::chrono::seconds(30));

		// Send the HTTP request to the remote host
		http::async_write(stream_, req_,
			beast::bind_front_handler(&http_session::on_write, shared_from_this()));
	}

	void on_write(beast::error_code err, std::size_t bytes_transferred)
	{
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
		boost::ignore_unused(bytes_transferred);
		if (err) {
			return report_failure(err, "read");
		}
		// Write the message to standard out
		std::cout << res_ << std::endl;

		// Gracefully close the socket
		stream_.socket().shutdown(tcp::socket::shutdown_both, err);

		// not_connected happens sometimes so don't bother reporting it.
		if (err && err != beast::errc::not_connected)
			return report_failure(err, "shutdown");

		// If we get here then the connection is closed gracefully
	}
};

static int input_params_error() {
	cerr <<
		"Usage: http_client --host=hostname --port=portnumber --keep-alive=seconds --post-request=seconds --reload=seconds --request=path_to_request.json" <<
		"Example:\n" <<
		"    http_client --host=www.example.com --port=80 --keep-alive=10 --post-request=20 --reload=30 --request=request.json";
	return EXIT_FAILURE;
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

	request_buffer req_buff(settings["request"].c_str());
	if (!req_buff.good()) {
		cerr << "bad path_to_request" << endl;
		return input_params_error();
	}
	return EXIT_SUCCESS;
}

