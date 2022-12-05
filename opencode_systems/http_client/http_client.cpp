// http_client.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include <boost/regex.hpp> 
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <map>

using namespace std;

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

	const auto request_path(settings["request"]);
	ifstream f(request_path.c_str());
	if (!f.good()) {
		cerr << "bad path_to_request" << endl;
		return input_params_error();
	}

	return EXIT_SUCCESS;
}

