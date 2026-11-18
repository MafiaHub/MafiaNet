#pragma once

#include <string>

// The sample master server takes a host and path in a single URI.
inline std::string MakeHttpRequest(const std::string &method, const std::string &uri, const std::string &contentType = {}, const std::string &body = {})
{
	const auto scheme = uri.find("://");
	const auto host = scheme == std::string::npos ? 0 : scheme + 3;
	const auto path = uri.find('/', host);
	if (path == host || path == std::string::npos)
		return {};
	std::string request = method + " " + uri.substr(path) + " HTTP/1.1\r\nHost: " + uri.substr(host, path - host) + "\r\n";
	if (method == "POST")
		request += "Content-Type: " + contentType + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
	if (method == "DELETE")
		request += "Content-Length: 0\r\nConnection: close\r\n";
	return request + "\r\n" + body;
}
