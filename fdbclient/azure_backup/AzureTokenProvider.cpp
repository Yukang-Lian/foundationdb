/*
 * AzureTokenProvider.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/AzureTokenProvider.h"

#ifdef BUILD_AZURE_BACKUP

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

#include <curl/curl.h>

#include "flow/Error.h"
#include "flow/UnitTest.h"
#include "fdbclient/json_spirit/json_spirit_reader_template.h"

namespace {

// The values a token can be requested for.  Azure Storage data-plane access always uses
// this resource/scope.
const char* const STORAGE_RESOURCE = "https%3A%2F%2Fstorage.azure.com%2F";
const char* const STORAGE_SCOPE = "https%3A%2F%2Fstorage.azure.com%2F.default";

const char* const IMDS_TOKEN_URL = "http://169.254.169.254/metadata/identity/oauth2/token?api-version=2018-02-01";
const char* const DEFAULT_AUTHORITY_HOST = "https://login.microsoftonline.com/";

std::string getEnvOrEmpty(const char* name) {
	const char* v = std::getenv(name);
	return v == nullptr ? std::string() : std::string(v);
}

size_t curlWriteCallback(char* data, size_t size, size_t nmemb, void* userdata) {
	static_cast<std::string*>(userdata)->append(data, size * nmemb);
	return size * nmemb;
}

void ensureCurlInitialized() {
	static std::once_flag once;
	std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct CurlHandle {
	CURL* h;
	curl_slist* headers = nullptr;
	CurlHandle() : h(curl_easy_init()) {}
	~CurlHandle() {
		if (headers != nullptr) {
			curl_slist_free_all(headers);
		}
		if (h != nullptr) {
			curl_easy_cleanup(h);
		}
	}
};

std::string urlEscape(CURL* h, const std::string& value) {
	char* escaped = curl_easy_escape(h, value.c_str(), value.size());
	if (escaped == nullptr) {
		throw backup_auth_missing();
	}
	std::string result = escaped;
	curl_free(escaped);
	return result;
}

// Performs the HTTP request and returns the response body.  Throws backup_auth_missing() on
// transport errors and non-200 responses.  These functions run on a background thread, so
// errors are reported to stderr (matching the Azure backup container's error reporting) and
// the caller is expected to trace the failure from the flow thread.
std::string performTokenRequest(CurlHandle& curl, const char* description) {
	std::string body;
	curl_easy_setopt(curl.h, CURLOPT_WRITEFUNCTION, curlWriteCallback);
	curl_easy_setopt(curl.h, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl.h, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl.h, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl.h, CURLOPT_FOLLOWLOCATION, 0L);

	CURLcode rc = curl_easy_perform(curl.h);
	if (rc != CURLE_OK) {
		fprintf(stderr, "fetch %s failed: %s\n", description, curl_easy_strerror(rc));
		throw backup_auth_missing();
	}
	long httpCode = 0;
	curl_easy_getinfo(curl.h, CURLINFO_RESPONSE_CODE, &httpCode);
	if (httpCode != 200) {
		// The response body of a failed token request describes the failure (e.g. identity
		// not found, permission denied) but can not contain a token, so it is safe to print.
		fprintf(stderr, "fetch %s failed: HTTP %ld: %s\n", description, httpCode, body.c_str());
		throw backup_auth_missing();
	}
	return body;
}

AzureAccessToken fetchManagedIdentityToken() {
	ensureCurlInitialized();
	CurlHandle curl;
	if (curl.h == nullptr) {
		throw backup_auth_missing();
	}

	// FDB_AZURE_IMDS_ENDPOINT overrides the token endpoint base, e.g. for environments where
	// the identity endpoint is injected (Azure App Service style) or for testing with a mock
	// token server.  The default is the standard IMDS address.
	std::string url;
	std::string endpointOverride = getEnvOrEmpty("FDB_AZURE_IMDS_ENDPOINT");
	if (!endpointOverride.empty()) {
		while (!endpointOverride.empty() && endpointOverride.back() == '/') {
			endpointOverride.pop_back();
		}
		url = endpointOverride + "/metadata/identity/oauth2/token?api-version=2018-02-01";
	} else {
		url = IMDS_TOKEN_URL;
	}
	url += "&resource=";
	url += STORAGE_RESOURCE;
	// An explicitly configured client id selects a user-assigned managed identity; without
	// it IMDS uses the system-assigned identity.
	std::string clientId = getEnvOrEmpty("FDB_AZURE_CLIENT_ID");
	if (!clientId.empty()) {
		url += "&client_id=" + urlEscape(curl.h, clientId);
	}

	curl.headers = curl_slist_append(curl.headers, "Metadata: true");
	curl_easy_setopt(curl.h, CURLOPT_HTTPHEADER, curl.headers);
	curl_easy_setopt(curl.h, CURLOPT_URL, url.c_str());
	// The IMDS endpoint is a link-local (or local mock) address which must never be reached
	// through an HTTP proxy configured in the environment.
	curl_easy_setopt(curl.h, CURLOPT_NOPROXY, "*");

	std::string body = performTokenRequest(curl, "Azure managed identity token from IMDS");
	return parseAzureTokenResponse(body);
}

AzureAccessToken fetchWorkloadIdentityToken() {
	ensureCurlInitialized();

	std::string clientId = getEnvOrEmpty("AZURE_CLIENT_ID");
	std::string tenantId = getEnvOrEmpty("AZURE_TENANT_ID");
	std::string tokenFile = getEnvOrEmpty("AZURE_FEDERATED_TOKEN_FILE");
	std::string authorityHost = getEnvOrEmpty("AZURE_AUTHORITY_HOST");
	if (authorityHost.empty()) {
		authorityHost = DEFAULT_AUTHORITY_HOST;
	}
	if (authorityHost.back() != '/') {
		authorityHost += '/';
	}
	if (clientId.empty() || tenantId.empty() || tokenFile.empty()) {
		fprintf(stderr,
		        "Azure workload identity requires the AZURE_CLIENT_ID, AZURE_TENANT_ID and "
		        "AZURE_FEDERATED_TOKEN_FILE environment variables\n");
		throw backup_auth_missing();
	}

	std::ifstream file(tokenFile);
	if (!file) {
		fprintf(stderr, "Could not read Azure federated token file %s\n", tokenFile.c_str());
		throw backup_auth_missing();
	}
	std::stringstream contents;
	contents << file.rdbuf();
	std::string assertion = contents.str();
	// Trim trailing whitespace/newlines from the projected token file.
	while (!assertion.empty() && (assertion.back() == '\n' || assertion.back() == '\r' || assertion.back() == ' ')) {
		assertion.pop_back();
	}
	if (assertion.empty()) {
		fprintf(stderr, "Azure federated token file %s is empty\n", tokenFile.c_str());
		throw backup_auth_missing();
	}

	CurlHandle curl;
	if (curl.h == nullptr) {
		throw backup_auth_missing();
	}

	std::string url = authorityHost + tenantId + "/oauth2/v2.0/token";
	std::string postBody;
	postBody += "grant_type=client_credentials";
	postBody += "&client_id=" + urlEscape(curl.h, clientId);
	postBody += "&scope=";
	postBody += STORAGE_SCOPE;
	postBody += "&client_assertion_type=urn%3Aietf%3Aparams%3Aoauth%3Aclient-assertion-type%3Ajwt-bearer";
	postBody += "&client_assertion=" + urlEscape(curl.h, assertion);

	curl_easy_setopt(curl.h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl.h, CURLOPT_POSTFIELDS, postBody.c_str());

	std::string body = performTokenRequest(curl, "Azure workload identity token");
	return parseAzureTokenResponse(body);
}

} // namespace

AzureAuthMode azureAuthModeFromEnvironment() {
	std::string mode = getEnvOrEmpty("FDB_AZURE_AUTH_MODE");
	if (mode.empty() || mode == "shared_key") {
		return AzureAuthMode::SHARED_KEY;
	}
	if (mode == "managed_identity") {
		return AzureAuthMode::MANAGED_IDENTITY;
	}
	if (mode == "workload_identity") {
		return AzureAuthMode::WORKLOAD_IDENTITY;
	}
	fprintf(stderr,
	        "Invalid FDB_AZURE_AUTH_MODE '%s', expected shared_key, managed_identity or workload_identity\n",
	        mode.c_str());
	throw backup_invalid_url();
}

AzureAccessToken fetchAzureStorageToken(AzureAuthMode mode) {
	switch (mode) {
	case AzureAuthMode::MANAGED_IDENTITY:
		return fetchManagedIdentityToken();
	case AzureAuthMode::WORKLOAD_IDENTITY:
		return fetchWorkloadIdentityToken();
	default:
		// SHARED_KEY does not use bearer tokens.
		throw backup_auth_missing();
	}
}

AzureAccessToken parseAzureTokenResponse(const std::string& jsonBody) {
	json_spirit::mValue json;
	if (!json_spirit::read_string(jsonBody, json) || json.type() != json_spirit::obj_type) {
		fprintf(stderr, "Azure token response is not a JSON object\n");
		throw backup_auth_missing();
	}
	const json_spirit::mObject& obj = json.get_obj();

	auto tokenIter = obj.find("access_token");
	if (tokenIter == obj.end() || tokenIter->second.type() != json_spirit::str_type ||
	    tokenIter->second.get_str().empty()) {
		fprintf(stderr, "Azure token response has no access_token\n");
		throw backup_auth_missing();
	}

	AzureAccessToken result;
	result.token = tokenIter->second.get_str();

	// IMDS returns expires_in as a JSON string, the AAD v2 endpoint as a JSON number.
	auto expiresIter = obj.find("expires_in");
	if (expiresIter == obj.end()) {
		fprintf(stderr, "Azure token response has no expires_in\n");
		throw backup_auth_missing();
	}
	if (expiresIter->second.type() == json_spirit::str_type) {
		result.expiresIn = atof(expiresIter->second.get_str().c_str());
	} else if (expiresIter->second.type() == json_spirit::int_type) {
		result.expiresIn = expiresIter->second.get_int64();
	} else if (expiresIter->second.type() == json_spirit::real_type) {
		result.expiresIn = expiresIter->second.get_real();
	} else {
		fprintf(stderr, "Azure token response expires_in has unexpected type\n");
		throw backup_auth_missing();
	}
	if (result.expiresIn <= 0) {
		fprintf(stderr, "Azure token response expires_in is not positive\n");
		throw backup_auth_missing();
	}
	return result;
}

TEST_CASE("/backup/containers/azure/token_response") {
	// IMDS style: expires_in is a JSON string.
	AzureAccessToken imds = parseAzureTokenResponse(
	    "{\"access_token\":\"tok1\",\"expires_in\":\"3599\",\"token_type\":\"Bearer\",\"resource\":\"x\"}");
	ASSERT(imds.token == "tok1");
	ASSERT_EQ((int)imds.expiresIn, 3599);

	// AAD v2 style: expires_in is a JSON number.
	AzureAccessToken aad = parseAzureTokenResponse("{\"token_type\":\"Bearer\",\"expires_in\":86399,\"ext_expires_in\":"
	                                               "86399,\"access_token\":\"tok2\"}");
	ASSERT(aad.token == "tok2");
	ASSERT_EQ((int)aad.expiresIn, 86399);

	// Malformed responses are rejected.
	for (const char* bad : { "",
	                         "not json",
	                         "[]",
	                         "{}",
	                         "{\"access_token\":\"\",\"expires_in\":10}",
	                         "{\"access_token\":\"t\"}",
	                         "{\"access_token\":\"t\",\"expires_in\":\"soon\"}",
	                         "{\"access_token\":\"t\",\"expires_in\":0}",
	                         "{\"expires_in\":100}" }) {
		try {
			parseAzureTokenResponse(bad);
			ASSERT(false);
		} catch (Error& e) {
			ASSERT_EQ(e.code(), error_code_backup_auth_missing);
		}
	}

	return Void();
}

#endif
