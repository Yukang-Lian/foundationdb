/*
 * S3BlobStore.actor.cpp
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

#include "fdbclient/S3BlobStore.h"

#include "flow/IConnection.h"
#include "md5/md5.h"
#include "libb64/encode.h"
#include "fdbclient/sha1/SHA1.h"
#include <atomic>
#include <climits>
#include <time.h>
#include <iomanip>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string.hpp>
#include "flow/IAsyncFile.h"
#include "fdbclient/AsyncTaskThread.h"
#include "fdbclient/GcpTokenProvider.h"
#include "flow/Hostname.h"
#include "flow/UnitTest.h"
#include "rapidxml/rapidxml.hpp"
#include "fdbclient/FDBAWSCredentialsProvider.h"

#include "flow/actorcompiler.h" // has to be last include

using namespace rapidxml;

json_spirit::mObject S3BlobStoreEndpoint::Stats::getJSON() {
	json_spirit::mObject o;

	o["requests_failed"] = requests_failed;
	o["requests_successful"] = requests_successful;
	o["bytes_sent"] = bytes_sent;

	return o;
}

S3BlobStoreEndpoint::Stats S3BlobStoreEndpoint::Stats::operator-(const Stats& rhs) {
	Stats r;
	r.requests_failed = requests_failed - rhs.requests_failed;
	r.requests_successful = requests_successful - rhs.requests_successful;
	r.bytes_sent = bytes_sent - rhs.bytes_sent;
	return r;
}

S3BlobStoreEndpoint::Stats S3BlobStoreEndpoint::s_stats;
std::unique_ptr<S3BlobStoreEndpoint::BlobStats> S3BlobStoreEndpoint::blobStats;
Future<Void> S3BlobStoreEndpoint::statsLogger = Never();

std::unordered_map<BlobStoreConnectionPoolKey, Reference<S3BlobStoreEndpoint::ConnectionPoolData>>
    S3BlobStoreEndpoint::globalConnectionPool;

S3BlobStoreEndpoint::BlobKnobs::BlobKnobs() {
	secure_connection = 1;
	connect_tries = CLIENT_KNOBS->BLOBSTORE_CONNECT_TRIES;
	connect_timeout = CLIENT_KNOBS->BLOBSTORE_CONNECT_TIMEOUT;
	max_connection_life = CLIENT_KNOBS->BLOBSTORE_MAX_CONNECTION_LIFE;
	request_tries = CLIENT_KNOBS->BLOBSTORE_REQUEST_TRIES;
	request_timeout_min = CLIENT_KNOBS->BLOBSTORE_REQUEST_TIMEOUT_MIN;
	requests_per_second = CLIENT_KNOBS->BLOBSTORE_REQUESTS_PER_SECOND;
	concurrent_requests = CLIENT_KNOBS->BLOBSTORE_CONCURRENT_REQUESTS;
	list_requests_per_second = CLIENT_KNOBS->BLOBSTORE_LIST_REQUESTS_PER_SECOND;
	write_requests_per_second = CLIENT_KNOBS->BLOBSTORE_WRITE_REQUESTS_PER_SECOND;
	read_requests_per_second = CLIENT_KNOBS->BLOBSTORE_READ_REQUESTS_PER_SECOND;
	delete_requests_per_second = CLIENT_KNOBS->BLOBSTORE_DELETE_REQUESTS_PER_SECOND;
	multipart_max_part_size = CLIENT_KNOBS->BLOBSTORE_MULTIPART_MAX_PART_SIZE;
	multipart_min_part_size = CLIENT_KNOBS->BLOBSTORE_MULTIPART_MIN_PART_SIZE;
	concurrent_uploads = CLIENT_KNOBS->BLOBSTORE_CONCURRENT_UPLOADS;
	concurrent_lists = CLIENT_KNOBS->BLOBSTORE_CONCURRENT_LISTS;
	concurrent_reads_per_file = CLIENT_KNOBS->BLOBSTORE_CONCURRENT_READS_PER_FILE;
	concurrent_writes_per_file = CLIENT_KNOBS->BLOBSTORE_CONCURRENT_WRITES_PER_FILE;
	enable_read_cache = CLIENT_KNOBS->BLOBSTORE_ENABLE_READ_CACHE;
	read_block_size = CLIENT_KNOBS->BLOBSTORE_READ_BLOCK_SIZE;
	read_ahead_blocks = CLIENT_KNOBS->BLOBSTORE_READ_AHEAD_BLOCKS;
	read_cache_blocks_per_file = CLIENT_KNOBS->BLOBSTORE_READ_CACHE_BLOCKS_PER_FILE;
	max_send_bytes_per_second = CLIENT_KNOBS->BLOBSTORE_MAX_SEND_BYTES_PER_SECOND;
	max_recv_bytes_per_second = CLIENT_KNOBS->BLOBSTORE_MAX_RECV_BYTES_PER_SECOND;
	max_delay_retryable_error = CLIENT_KNOBS->BLOBSTORE_MAX_DELAY_RETRYABLE_ERROR;
	max_delay_connection_failed = CLIENT_KNOBS->BLOBSTORE_MAX_DELAY_CONNECTION_FAILED;
	sdk_auth = false;
	gcp_auth = false;
	aliyun_auth = false;
	global_connection_pool = CLIENT_KNOBS->BLOBSTORE_GLOBAL_CONNECTION_POOL;
}

bool S3BlobStoreEndpoint::BlobKnobs::set(StringRef name, int value) {
#define TRY_PARAM(n, sn)                                                                                               \
	if (name == #n || name == #sn) {                                                                                   \
		n = value;                                                                                                     \
		return true;                                                                                                   \
	}
	TRY_PARAM(secure_connection, sc)
	TRY_PARAM(connect_tries, ct);
	TRY_PARAM(connect_timeout, cto);
	TRY_PARAM(max_connection_life, mcl);
	TRY_PARAM(request_tries, rt);
	TRY_PARAM(request_timeout_min, rtom);
	// TODO: For backward compatibility because request_timeout was renamed to request_timeout_min
	if (name == "request_timeout"_sr || name == "rto"_sr) {
		request_timeout_min = value;
		return true;
	}
	TRY_PARAM(requests_per_second, rps);
	TRY_PARAM(list_requests_per_second, lrps);
	TRY_PARAM(write_requests_per_second, wrps);
	TRY_PARAM(read_requests_per_second, rrps);
	TRY_PARAM(delete_requests_per_second, drps);
	TRY_PARAM(concurrent_requests, cr);
	TRY_PARAM(multipart_max_part_size, maxps);
	TRY_PARAM(multipart_min_part_size, minps);
	TRY_PARAM(concurrent_uploads, cu);
	TRY_PARAM(concurrent_lists, cl);
	TRY_PARAM(concurrent_reads_per_file, crpf);
	TRY_PARAM(concurrent_writes_per_file, cwpf);
	TRY_PARAM(enable_read_cache, erc);
	TRY_PARAM(read_block_size, rbs);
	TRY_PARAM(read_ahead_blocks, rab);
	TRY_PARAM(read_cache_blocks_per_file, rcb);
	TRY_PARAM(max_send_bytes_per_second, sbps);
	TRY_PARAM(max_recv_bytes_per_second, rbps);
	TRY_PARAM(max_delay_retryable_error, dre);
	TRY_PARAM(max_delay_connection_failed, dcf);
	TRY_PARAM(sdk_auth, sa);
	TRY_PARAM(gcp_auth, ga);
	TRY_PARAM(aliyun_auth, aa);
	TRY_PARAM(global_connection_pool, gcp);
#undef TRY_PARAM
	return false;
}

// Returns an S3 Blob URL parameter string that specifies all of the non-default options for the endpoint using option
// short names.
std::string S3BlobStoreEndpoint::BlobKnobs::getURLParameters() const {
	static BlobKnobs defaults;
	std::string r;
#define _CHECK_PARAM(n, sn)                                                                                            \
	if (n != defaults.n) {                                                                                             \
		r += format("%s%s=%d", r.empty() ? "" : "&", #sn, n);                                                          \
	}
	_CHECK_PARAM(secure_connection, sc);
	_CHECK_PARAM(connect_tries, ct);
	_CHECK_PARAM(connect_timeout, cto);
	_CHECK_PARAM(max_connection_life, mcl);
	_CHECK_PARAM(request_tries, rt);
	_CHECK_PARAM(request_timeout_min, rto);
	_CHECK_PARAM(requests_per_second, rps);
	_CHECK_PARAM(list_requests_per_second, lrps);
	_CHECK_PARAM(write_requests_per_second, wrps);
	_CHECK_PARAM(read_requests_per_second, rrps);
	_CHECK_PARAM(delete_requests_per_second, drps);
	_CHECK_PARAM(concurrent_requests, cr);
	_CHECK_PARAM(multipart_max_part_size, maxps);
	_CHECK_PARAM(multipart_min_part_size, minps);
	_CHECK_PARAM(concurrent_uploads, cu);
	_CHECK_PARAM(concurrent_lists, cl);
	_CHECK_PARAM(concurrent_reads_per_file, crpf);
	_CHECK_PARAM(concurrent_writes_per_file, cwpf);
	_CHECK_PARAM(enable_read_cache, erc);
	_CHECK_PARAM(read_block_size, rbs);
	_CHECK_PARAM(read_ahead_blocks, rab);
	_CHECK_PARAM(read_cache_blocks_per_file, rcb);
	_CHECK_PARAM(max_send_bytes_per_second, sbps);
	_CHECK_PARAM(max_recv_bytes_per_second, rbps);
	_CHECK_PARAM(sdk_auth, sa);
	_CHECK_PARAM(gcp_auth, ga);
	_CHECK_PARAM(aliyun_auth, aa);
	_CHECK_PARAM(global_connection_pool, gcp);
	_CHECK_PARAM(max_delay_retryable_error, dre);
	_CHECK_PARAM(max_delay_connection_failed, dcf);
#undef _CHECK_PARAM
	return r;
}

std::string guessRegionFromDomain(std::string domain) {
	static const std::vector<const char*> knownServices = { "s3.", "cos.", "oss-", "obs." };
	boost::algorithm::to_lower(domain);

	for (int i = 0; i < knownServices.size(); ++i) {
		const char* service = knownServices[i];

		std::size_t p = domain.find(service);
		if (p == std::string::npos || (p >= 1 && domain[p - 1] != '.')) {
			// eg. 127.0.0.1, example.com, s3-service.example.com, mys3.example.com
			continue;
		}

		StringRef h = StringRef(domain).substr(p);

		if (!h.startsWith("oss-"_sr)) {
			h.eat(service); // ignore s3 service
		}

		return h.eat(".").toString();
	}

	return "";
}

Reference<S3BlobStoreEndpoint> S3BlobStoreEndpoint::fromString(const std::string& url,
                                                               const Optional<std::string>& proxy,
                                                               std::string* resourceFromURL,
                                                               std::string* error,
                                                               ParametersT* ignored_parameters) {
	if (resourceFromURL)
		resourceFromURL->clear();

	try {
		StringRef t(url);
		StringRef prefix = t.eat("://");
		if (prefix != "blobstore"_sr)
			throw format("Invalid blobstore URL prefix '%s'", prefix.toString().c_str());

		Optional<std::string> proxyHost, proxyPort;
		if (proxy.present()) {
			StringRef proxyRef(proxy.get());
			if (proxy.get().find("://") != std::string::npos) {
				StringRef proxyPrefix = proxyRef.eat("://");
				if (proxyPrefix != "http"_sr) {
					throw format("Invalid proxy URL prefix '%s'. Either don't use a prefix, or use http://",
					             proxyPrefix.toString().c_str());
				}
			}
			std::string proxyBody = proxyRef.eat().toString();
			if (!Hostname::isHostname(proxyBody) && !NetworkAddress::parseOptional(proxyBody).present()) {
				throw format("'%s' is not a valid value for proxy. Format should be either IP:port or host:port.",
				             proxyBody.c_str());
			}
			StringRef p(proxyBody);
			proxyHost = p.eat(":").toString();
			proxyPort = p.eat().toString();
		}

		// Credentials precede the host as <key>:<secret>[:<token>]@. An '@' after the start of the query string
		// belongs to a parameter value, such as the service account email of gcp_impersonation_service_account.
		Optional<StringRef> cred;
		size_t atPos = url.find("@");
		size_t queryPos = url.find("?");
		if (atPos != std::string::npos && (queryPos == std::string::npos || atPos < queryPos)) {
			cred = t.eat("@");
		}
		uint8_t foundSeparator = 0;
		StringRef hostPort = t.eatAny("/?", &foundSeparator);
		StringRef resource;
		if (foundSeparator == '/') {
			resource = t.eat("?");
		}

		// hostPort is at least a host or IP address, optionally followed by :portNumber or :serviceName
		StringRef h(hostPort);
		StringRef host = h.eat(":");
		if (host.size() == 0)
			throw std::string("host cannot be empty");

		StringRef service = h.eat();

		std::string region = guessRegionFromDomain(host.toString());

		BlobKnobs knobs;
		HTTP::Headers extraHeaders;
		GcpCredentialConfig gcpCredentials;
		bool gcpCredentialParams = false;
		AwsCredentialConfig awsCredentials;
		bool awsCredentialParams = false;
		AliyunCredentialConfig aliyunCredentials;
		bool aliyunCredentialParams = false;
		while (1) {
			StringRef name = t.eat("=");
			if (name.size() == 0)
				break;
			StringRef value = t.eat("&");

			// Special case for header
			if (name == "header"_sr) {
				StringRef originalValue = value;
				StringRef headerFieldName = value.eat(":");
				StringRef headerFieldValue = value;
				if (headerFieldName.size() == 0 || headerFieldValue.size() == 0) {
					throw format("'%s' is not a valid value for '%s' parameter.  Format is <FieldName>:<FieldValue> "
					             "where strings are not empty.",
					             originalValue.toString().c_str(),
					             name.toString().c_str());
				}
				std::string& fieldValue = extraHeaders[headerFieldName.toString()];
				// RFC 2616 section 4.2 says header field names can repeat but only if it is valid to concatenate their
				// values with comma separation
				if (!fieldValue.empty()) {
					fieldValue.append(",");
				}
				fieldValue.append(headerFieldValue.toString());
				continue;
			}

			// overwrite s3 region from parameter
			if (name == "region"_sr) {
				region = value.toString();
				continue;
			}

			// Google Cloud credential settings. String valued, so not knobs; they mirror the
			// gcs.credential_provider_type and gcs.impersonation_service_account settings of Doris.
			if (name == "gcp_credential_provider_type"_sr) {
				if (!parseGcpCredentialProviderType(value.toString(), &gcpCredentials.providerType)) {
					throw format("'%s' is not a valid value for gcp_credential_provider_type, expected DEFAULT or "
					             "COMPUTE_ENGINE",
					             value.toString().c_str());
				}
				gcpCredentialParams = true;
				continue;
			}
			// AWS credential settings for sdk_auth. String valued, so not knobs; they mirror the s3.role_arn,
			// s3.external_id and s3.credentials_provider_type settings of Doris.
			if (name == "role_arn"_sr) {
				if (value.toString().find("arn:") != 0) {
					throw format("'%s' is not a valid value for role_arn, expected an IAM role ARN such as "
					             "arn:aws:iam::123456789012:role/name",
					             value.toString().c_str());
				}
				awsCredentials.roleArn = value.toString();
				awsCredentialParams = true;
				continue;
			}
			if (name == "external_id"_sr) {
				awsCredentials.externalId = value.toString();
				awsCredentialParams = true;
				continue;
			}
			if (name == "credentials_provider_type"_sr) {
				if (!parseAwsCredentialProviderType(value.toString(), &awsCredentials.providerType)) {
					throw format("'%s' is not a valid value for credentials_provider_type, expected DEFAULT, ENV, "
					             "SYSTEM_PROPERTIES, WEB_IDENTITY, CONTAINER or INSTANCE_PROFILE",
					             value.toString().c_str());
				}
				awsCredentialParams = true;
				continue;
			}
			// Alibaba Cloud credential setting for aliyun_auth. BE has no keyless mode for OSS yet; the value names
			// follow the Alibaba Cloud Credentials SDK.
			if (name == "aliyun_credential_provider_type"_sr) {
				if (!parseAliyunCredentialProviderType(value.toString(), &aliyunCredentials.providerType)) {
					throw format("'%s' is not a valid value for aliyun_credential_provider_type, expected DEFAULT, "
					             "ENV, ECS_RAM_ROLE or OIDC_ROLE_ARN",
					             value.toString().c_str());
				}
				aliyunCredentialParams = true;
				continue;
			}
			if (name == "gcp_impersonation_service_account"_sr) {
				// accept the '@' either literally or URL-encoded as %40
				std::string account = value.toString();
				size_t encoded = account.find("%40");
				if (encoded != std::string::npos) {
					account.replace(encoded, 3, "@");
				}
				if (!isValidGcpServiceAccountEmail(account)) {
					throw format("'%s' is not a valid value for gcp_impersonation_service_account, expected a service "
					             "account email such as name@project.iam.gserviceaccount.com",
					             value.toString().c_str());
				}
				gcpCredentials.impersonationServiceAccount = account;
				gcpCredentialParams = true;
				continue;
			}

			// See if the parameter is a knob
			// First try setting a dummy value (all knobs are currently numeric) just to see if this parameter is known
			// to S3BlobStoreEndpoint. If it is, then we will set it to a good value or throw below, so the dummy set
			// has no bad side effects.
			bool known = knobs.set(name, 0);

			// If the parameter is not known to S3BlobStoreEndpoint then throw unless there is an ignored_parameters set
			// to add it to
			if (!known) {
				if (ignored_parameters == nullptr) {
					throw format("%s is not a valid parameter name", name.toString().c_str());
				}
				(*ignored_parameters)[name.toString()] = value.toString();
				continue;
			}

			// The parameter is known to S3BlobStoreEndpoint so it must be numeric and valid.
			char* valueEnd = nullptr;
			std::string s = value.toString();
			long int ivalue = strtol(s.c_str(), &valueEnd, 10);
			if (*valueEnd || (ivalue == 0 && s != "0") ||
			    (((ivalue == LONG_MAX) || (ivalue == LONG_MIN)) && errno == ERANGE))
				throw format("%s is not a valid value for %s", s.c_str(), name.toString().c_str());

			// It should not be possible for this set to fail now since the dummy set above had to have worked.
			ASSERT(knobs.set(name, ivalue));
		}

		if (resourceFromURL != nullptr)
			*resourceFromURL = resource.toString();

		Optional<S3BlobStoreEndpoint::Credentials> creds;
		if (cred.present()) {
			StringRef c(cred.get());
			StringRef key = c.eat(":");
			StringRef secret = c.eat(":");
			StringRef securityToken = c.eat();
			creds = S3BlobStoreEndpoint::Credentials{ key.toString(), secret.toString(), securityToken.toString() };
		}

		// The Google Cloud credential parameters select token auth by themselves, as gcs.credential_provider_type
		// does in Doris.
		if (gcpCredentialParams) {
			knobs.gcp_auth = 1;
		}
		if (knobs.gcp_auth && creds.present()) {
			throw std::string(
			    "gcp_auth authenticates with Google Cloud access tokens, remove the credentials from the URL");
		}
		// The AWS credential parameters select sdk_auth by themselves, as role_arn does in Doris, and like there
		// they cannot be combined with access keys.
		if (awsCredentialParams) {
			if (!awsCredentials.externalId.empty() && awsCredentials.roleArn.empty()) {
				throw std::string("external_id requires role_arn");
			}
			if (creds.present()) {
				throw std::string("role_arn, external_id and credentials_provider_type resolve credentials with the "
				                  "AWS SDK, remove the credentials from the URL");
			}
			if (knobs.gcp_auth) {
				throw std::string("role_arn, external_id and credentials_provider_type are AWS settings and cannot be "
				                  "combined with gcp_auth");
			}
			knobs.sdk_auth = 1;
		}
		// The Alibaba Cloud credential parameter selects aliyun_auth by itself; the SDK-resolved credentials sign
		// requests like AK/SK, so the region is still needed and the URL must not carry keys.
		if (aliyunCredentialParams) {
			knobs.aliyun_auth = 1;
		}
		if (knobs.aliyun_auth) {
			if (creds.present()) {
				throw std::string(
				    "aliyun_auth resolves credentials with the Alibaba Cloud SDK, remove the credentials from the URL");
			}
			if (knobs.gcp_auth || knobs.sdk_auth) {
				throw std::string("aliyun_auth cannot be combined with gcp_auth, sdk_auth or their parameters");
			}
		}

		// Bearer token auth does not sign requests, so it needs no region.
		if (region.empty() && CLIENT_KNOBS->HTTP_REQUEST_AWS_V4_HEADER && !knobs.gcp_auth) {
			throw std::string(
			    "Failed to get region from host or parameter in url, region is required for aws v4 signature");
		}

		Reference<S3BlobStoreEndpoint> endpoint = makeReference<S3BlobStoreEndpoint>(
		    host.toString(), service.toString(), region, proxyHost, proxyPort, creds, knobs, extraHeaders);
		endpoint->gcpCredentials = gcpCredentials;
		endpoint->awsCredentials = awsCredentials;
		endpoint->awsCredentials.region = region;
		endpoint->aliyunCredentials = aliyunCredentials;
		return endpoint;

	} catch (std::string& err) {
		if (error != nullptr)
			*error = err;
		TraceEvent(SevWarnAlways, "S3BlobStoreEndpointBadURL")
		    .suppressFor(60)
		    .detail("Description", err)
		    .detail("Format", getURLFormat())
		    .detail("URL", url);
		throw backup_invalid_url();
	}
}

std::string S3BlobStoreEndpoint::getResourceURL(std::string resource, std::string params) const {
	std::string hostPort = host;
	if (!service.empty()) {
		hostPort.append(":");
		hostPort.append(service);
	}

	// If secret isn't being looked up from credentials files then it was passed explicitly in the URL so show it here.
	// Credentials resolved by the cloud SDKs (sdk_auth, gcp_auth) never appear: they are temporary and belong to the
	// machine's identity, not to the URL.
	std::string credsString;
	if (credentials.present() && !knobs.sdk_auth && !knobs.gcp_auth && !knobs.aliyun_auth) {
		if (!lookupKey) {
			credsString = credentials.get().key;
		}
		if (!lookupSecret) {
			credsString += ":" + credentials.get().secret;
			if (!credentials.get().securityToken.empty()) {
				credsString += ":" + credentials.get().securityToken;
			}
		}
		credsString += "@";
	}

	std::string r = format("blobstore://%s%s/%s", credsString.c_str(), hostPort.c_str(), resource.c_str());

	// Get params that are deviations from knob defaults
	std::string knobParams = knobs.getURLParameters();
	if (!knobParams.empty()) {
		if (!params.empty()) {
			params.append("&");
		}
		params.append(knobParams);
	}

	// AWS credential settings that deviate from their defaults
	if (knobs.sdk_auth) {
		if (awsCredentials.providerType != AwsCredentialProviderType::DEFAULT) {
			if (!params.empty()) {
				params.append("&");
			}
			params.append("credentials_provider_type=");
			params.append(awsCredentialProviderTypeName(awsCredentials.providerType));
		}
		if (!awsCredentials.roleArn.empty()) {
			if (!params.empty()) {
				params.append("&");
			}
			params.append("role_arn=");
			params.append(awsCredentials.roleArn);
		}
		if (!awsCredentials.externalId.empty()) {
			if (!params.empty()) {
				params.append("&");
			}
			params.append("external_id=");
			params.append(awsCredentials.externalId);
		}
	}

	// Alibaba Cloud credential settings that deviate from their defaults
	if (knobs.aliyun_auth && aliyunCredentials.providerType != AliyunCredentialProviderType::DEFAULT) {
		if (!params.empty()) {
			params.append("&");
		}
		params.append("aliyun_credential_provider_type=");
		params.append(aliyunCredentialProviderTypeName(aliyunCredentials.providerType));
	}

	// Google Cloud credential settings that deviate from their defaults
	if (knobs.gcp_auth) {
		if (gcpCredentials.providerType != GcpCredentialProviderType::DEFAULT) {
			if (!params.empty()) {
				params.append("&");
			}
			params.append("gcp_credential_provider_type=");
			params.append(gcpCredentialProviderTypeName(gcpCredentials.providerType));
		}
		if (!gcpCredentials.impersonationServiceAccount.empty()) {
			if (!params.empty()) {
				params.append("&");
			}
			params.append("gcp_impersonation_service_account=");
			params.append(gcpCredentials.impersonationServiceAccount);
		}
	}

	for (const auto& [k, v] : extraHeaders) {
		if (!params.empty()) {
			params.append("&");
		}
		params.append("header=");
		params.append(k);
		params.append(":");
		params.append(v);
	}

	if (!params.empty())
		r.append("?").append(params);

	return r;
}

std::string constructResourcePath(Reference<S3BlobStoreEndpoint> b,
                                  const std::string& bucket,
                                  const std::string& object) {
	std::string resource;

	if (b->getHost().find(bucket + ".") != 0) {
		resource += std::string("/") + bucket; // not virtual hosting mode
	}

	if (!object.empty()) {
		resource += "/";
		resource += object;
	}

	return resource;
}

ACTOR Future<bool> bucketExists_impl(Reference<S3BlobStoreEndpoint> b, std::string bucket) {
	wait(b->requestRateRead->getAllowance(1));

	std::string resource = constructResourcePath(b, bucket, "");
	HTTP::Headers headers;

	Reference<HTTP::IncomingResponse> r = wait(b->doRequest("HEAD", resource, headers, nullptr, 0, { 200, 404 }));
	return r->code == 200;
}

Future<bool> S3BlobStoreEndpoint::bucketExists(std::string const& bucket) {
	return bucketExists_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket);
}

ACTOR Future<bool> objectExists_impl(Reference<S3BlobStoreEndpoint> b, std::string bucket, std::string object) {
	wait(b->requestRateRead->getAllowance(1));

	std::string resource = constructResourcePath(b, bucket, object);
	HTTP::Headers headers;

	Reference<HTTP::IncomingResponse> r = wait(b->doRequest("HEAD", resource, headers, nullptr, 0, { 200, 404 }));
	return r->code == 200;
}

Future<bool> S3BlobStoreEndpoint::objectExists(std::string const& bucket, std::string const& object) {
	return objectExists_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object);
}

ACTOR Future<Void> deleteObject_impl(Reference<S3BlobStoreEndpoint> b, std::string bucket, std::string object) {
	wait(b->requestRateDelete->getAllowance(1));

	std::string resource = constructResourcePath(b, bucket, object);
	HTTP::Headers headers;
	// 200 or 204 means object successfully deleted, 404 means it already doesn't exist, so any of those are considered
	// successful
	Reference<HTTP::IncomingResponse> r =
	    wait(b->doRequest("DELETE", resource, headers, nullptr, 0, { 200, 204, 404 }));

	// But if the object already did not exist then the 'delete' is assumed to be successful but a warning is logged.
	if (r->code == 404) {
		TraceEvent(SevWarnAlways, "S3BlobStoreEndpointDeleteObjectMissing")
		    .detail("Host", b->host)
		    .detail("Bucket", bucket)
		    .detail("Object", object);
	}

	return Void();
}

Future<Void> S3BlobStoreEndpoint::deleteObject(std::string const& bucket, std::string const& object) {
	return deleteObject_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object);
}

ACTOR Future<Void> deleteRecursively_impl(Reference<S3BlobStoreEndpoint> b,
                                          std::string bucket,
                                          std::string prefix,
                                          int* pNumDeleted,
                                          int64_t* pBytesDeleted) {
	state PromiseStream<S3BlobStoreEndpoint::ListResult> resultStream;
	// Start a recursive parallel listing which will send results to resultStream as they are received
	state Future<Void> done = b->listObjectsStream(bucket, resultStream, prefix, '/', std::numeric_limits<int>::max());
	// Wrap done in an actor which will send end_of_stream since listObjectsStream() does not (so that many calls can
	// write to the same stream)
	done = map(done, [=](Void) mutable {
		resultStream.sendError(end_of_stream());
		return Void();
	});

	state std::list<Future<Void>> deleteFutures;
	try {
		loop {
			choose {
				// Throw if done throws, otherwise don't stop until end_of_stream
				when(wait(done)) {
					done = Never();
				}

				when(S3BlobStoreEndpoint::ListResult list = waitNext(resultStream.getFuture())) {
					for (auto& object : list.objects) {
						deleteFutures.push_back(map(b->deleteObject(bucket, object.name), [=](Void) -> Void {
							if (pNumDeleted != nullptr) {
								++*pNumDeleted;
							}
							if (pBytesDeleted != nullptr) {
								*pBytesDeleted += object.size;
							}
							return Void();
						}));
					}
				}
			}

			// This is just a precaution to avoid having too many outstanding delete actors waiting to run
			while (deleteFutures.size() > CLIENT_KNOBS->BLOBSTORE_CONCURRENT_REQUESTS) {
				wait(deleteFutures.front());
				deleteFutures.pop_front();
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream)
			throw;
	}

	while (deleteFutures.size() > 0) {
		wait(deleteFutures.front());
		deleteFutures.pop_front();
	}

	return Void();
}

Future<Void> S3BlobStoreEndpoint::deleteRecursively(std::string const& bucket,
                                                    std::string prefix,
                                                    int* pNumDeleted,
                                                    int64_t* pBytesDeleted) {
	return deleteRecursively_impl(
	    Reference<S3BlobStoreEndpoint>::addRef(this), bucket, prefix, pNumDeleted, pBytesDeleted);
}

ACTOR Future<Void> createBucket_impl(Reference<S3BlobStoreEndpoint> b, std::string bucket) {
	wait(b->requestRateWrite->getAllowance(1));

	bool exists = wait(b->bucketExists(bucket));
	if (!exists) {
		std::string resource = constructResourcePath(b, bucket, "");
		HTTP::Headers headers;

		std::string region = b->getRegion();
		if (region.empty()) {
			Reference<HTTP::IncomingResponse> r =
			    wait(b->doRequest("PUT", resource, headers, nullptr, 0, { 200, 409 }));
		} else {
			UnsentPacketQueue packets;
			StringRef body(format("<CreateBucketConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
			                      "  <LocationConstraint>%s</LocationConstraint>"
			                      "</CreateBucketConfiguration>",
			                      region.c_str()));
			PacketWriter pw(packets.getWriteBuffer(), nullptr, Unversioned());
			pw.serializeBytes(body);

			Reference<HTTP::IncomingResponse> r =
			    wait(b->doRequest("PUT", resource, headers, &packets, body.size(), { 200, 409 }));
		}
	}
	return Void();
}

Future<Void> S3BlobStoreEndpoint::createBucket(std::string const& bucket) {
	return createBucket_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket);
}

ACTOR Future<int64_t> objectSize_impl(Reference<S3BlobStoreEndpoint> b, std::string bucket, std::string object) {
	wait(b->requestRateRead->getAllowance(1));

	std::string resource = constructResourcePath(b, bucket, object);
	HTTP::Headers headers;

	Reference<HTTP::IncomingResponse> r = wait(b->doRequest("HEAD", resource, headers, nullptr, 0, { 200, 404 }));
	if (r->code == 404)
		throw file_not_found();
	return r->data.contentLen;
}

Future<int64_t> S3BlobStoreEndpoint::objectSize(std::string const& bucket, std::string const& object) {
	return objectSize_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object);
}

// Try to read a file, parse it as JSON, and return the resulting document.
// It will NOT throw if any errors are encountered, it will just return an empty
// JSON object and will log trace events for the errors encountered.
ACTOR Future<Optional<json_spirit::mObject>> tryReadJSONFile(std::string path) {
	state std::string content;

	// Event type to be logged in the event of an exception
	state const char* errorEventType = "BlobCredentialFileError";

	try {
		state Reference<IAsyncFile> f = wait(IAsyncFileSystem::filesystem()->open(
		    path, IAsyncFile::OPEN_NO_AIO | IAsyncFile::OPEN_READONLY | IAsyncFile::OPEN_UNCACHED, 0));
		state int64_t size = wait(f->size());
		state Standalone<StringRef> buf = makeString(size);
		int r = wait(f->read(mutateString(buf), size, 0));
		ASSERT(r == size);
		content = buf.toString();

		// Any exceptions from hehre forward are parse failures
		errorEventType = "BlobCredentialFileParseFailed";
		json_spirit::mValue json;
		json_spirit::read_string(content, json);
		if (json.type() == json_spirit::obj_type)
			return json.get_obj();
		else
			TraceEvent(SevWarn, "BlobCredentialFileNotJSONObject").suppressFor(60).detail("File", path);

	} catch (Error& e) {
		if (e.code() != error_code_actor_cancelled)
			TraceEvent(SevWarn, errorEventType).errorUnsuppressed(e).suppressFor(60).detail("File", path);
	}

	return Optional<json_spirit::mObject>();
}

// If the credentials expire, the connection will eventually fail and be discarded from the pool, and then a new
// connection will be constructed, which will call this again to get updated credentials.
// Runs on the credential thread (the SDK blocks on the metadata service or STS), so no TraceEvents here.
static S3BlobStoreEndpoint::Credentials getSecretSdk(const AwsCredentialConfig& config) {
#ifdef WITH_AWS_BACKUP
	Aws::Auth::AWSCredentials awsCreds = FDBAWSCredentialsProvider::getAwsCredentials(config);
	if (awsCreds.IsEmpty()) {
		throw backup_auth_missing();
	}

	S3BlobStoreEndpoint::Credentials fdbCreds;
	fdbCreds.key = awsCreds.GetAWSAccessKeyId();
	fdbCreds.secret = awsCreds.GetAWSSecretKey();
	fdbCreds.securityToken = awsCreds.GetSessionToken();
	return fdbCreds;
#else
	throw backup_auth_missing();
#endif
}

// The cloud SDKs block while resolving credentials, so they run on a dedicated thread shared by all endpoints
// of the process. Allocated once and never destroyed: tearing it down during static destruction would race the
// network's own shutdown.
AsyncTaskThread& credentialThread() {
	static AsyncTaskThread* thread = new AsyncTaskThread();
	return *thread;
}

// A fetch that never returns (an SDK bug, a wedged HTTP client) keeps the credential thread and so every later
// fetch of the process, which only shows as endless request timeouts while the backup silently stalls. The
// thread records when a fetch starts and finishes; when a new fetch is requested while one has been running far
// longer than the SDKs' own timeouts allow, the process reports it and exits so its supervisor restarts it with
// fresh SDK state.
namespace {
constexpr double CREDENTIAL_FETCH_STUCK_SECONDS = 300;
std::atomic<double> credentialFetchStartedAt{ 0 }; // timer_monotonic() of the running fetch, 0 when idle
} // namespace

template <class F>
Future<decltype(std::declval<F>()())> fetchCredentials(const F& func) {
	double startedAt = credentialFetchStartedAt.load();
	if (startedAt != 0 && timer_monotonic() - startedAt > CREDENTIAL_FETCH_STUCK_SECONDS) {
		criticalError(FDB_EXIT_ERROR,
		              "CredentialFetchStuck",
		              format("A cloud credential fetch has been running for %.0f seconds, so the credential thread "
		                     "is stuck and no blobstore connection can be authenticated. Exiting so the process "
		                     "can be restarted.",
		                     timer_monotonic() - startedAt)
		                  .c_str());
	}
	return credentialThread().execAsync([func] {
		credentialFetchStartedAt.store(timer_monotonic());
		try {
			auto result = func();
			credentialFetchStartedAt.store(0);
			return result;
		} catch (...) {
			credentialFetchStartedAt.store(0);
			throw;
		}
	});
}

// Obtains a bearer token for Google Cloud Storage through the Google Cloud SDK, from the source credential
// selected by gcp_credential_provider_type (application default credentials or only the GCE/GKE metadata server),
// optionally exchanged for a token of the impersonated service account.
#ifdef WITH_GCP_BACKUP
ACTOR Future<Void> refreshBearerToken_impl(Reference<S3BlobStoreEndpoint> b) {
	state int attempt = 0;
	state GcpCredentialConfig config = b->gcpCredentials;
	loop {
		try {
			// a plain copy for the lambda: state variables live in the actor object and cannot be captured
			GcpCredentialConfig cfg = config;
			GcpAccessToken token = wait(fetchCredentials([cfg] { return fetchGcpStorageToken(cfg); }));
			b->bearerToken = token.token;
			b->bearerTokenExpiration = now() + token.expiresIn;
			b->bearerTokenRefreshNotBefore = now() + 30;
			TraceEvent("S3BlobStoreGcpTokenRefreshed")
			    .detail("ExpiresIn", token.expiresIn)
			    .detail("CredentialProviderType", gcpCredentialProviderTypeName(config.providerType))
			    .detail("ImpersonationServiceAccount", config.impersonationServiceAccount);
			return Void();
		} catch (Error& e) {
			if (e.code() == error_code_actor_cancelled) {
				throw;
			}
			++attempt;
			TraceEvent(SevWarn, "S3BlobStoreGcpTokenRefreshFailed").errorUnsuppressed(e).detail("Attempt", attempt);
			if (attempt >= 3) {
				fprintf(stderr,
				        "ERROR: Unable to obtain Google Cloud credentials for gcp_auth (%s, provider %s%s%s). This "
				        "needs the GCE/GKE metadata server of a service account or, with DEFAULT, "
				        "GOOGLE_APPLICATION_CREDENTIALS; impersonation also needs roles/iam.serviceAccountTokenCreator "
				        "on the target service account.\n",
				        e.what(),
				        gcpCredentialProviderTypeName(config.providerType),
				        config.impersonationServiceAccount.empty() ? "" : ", impersonating ",
				        config.impersonationServiceAccount.c_str());
				throw backup_auth_missing();
			}
			wait(delay(attempt));
		}
	}
}
#else
Future<Void> refreshBearerToken_impl(Reference<S3BlobStoreEndpoint> b) {
	TraceEvent(SevError, "S3BlobStoreNoGcpSDK");
	fprintf(stderr, "ERROR: gcp_auth=1 requires a build with BUILD_GCP_BACKUP=ON.\n");
	return backup_auth_missing();
}
#endif

Future<Void> S3BlobStoreEndpoint::ensureBearerToken() {
	// Refresh once less than 5 minutes remain, which is also when the metadata server starts handing out a new token,
	// but never more often than every 30 seconds in case the SDK keeps returning its cached token near expiry.
	if (!bearerToken.empty() && (bearerTokenExpiration - now() > 300 || now() < bearerTokenRefreshNotBefore)) {
		return Void();
	}
	// Share one in-flight refresh between concurrent requests. A finished refresh, successful or not, is replaced.
	if (!bearerTokenRefresh.isValid() || bearerTokenRefresh.isReady()) {
		bearerTokenRefresh = refreshBearerToken_impl(Reference<S3BlobStoreEndpoint>::addRef(this));
	}
	return bearerTokenRefresh;
}

void S3BlobStoreEndpoint::setBearerAuthHeaders(HTTP::Headers& headers) {
	// The GCS XML API requires a Date header on every request.
	char dateBuf[64];
	time_t ts;
	time(&ts);
	strftime(dateBuf, 64, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&ts));
	headers["Date"] = dateBuf;
	headers["Authorization"] = "Bearer " + bearerToken;
}

// Runs on the credential thread (the SDK blocks on the metadata service or STS), so no TraceEvents here.
static AliyunCredentials getSecretAliyun(const AliyunCredentialConfig& config) {
#ifdef WITH_ALIYUN_BACKUP
	return fetchAliyunCredentials(config);
#else
	fprintf(stderr, "ERROR: aliyun_auth requires a build with BUILD_ALIYUN_BACKUP=ON.\n");
	throw backup_auth_missing();
#endif
}

ACTOR Future<Void> updateSecret_impl(Reference<S3BlobStoreEndpoint> b) {
	if (b->knobs.gcp_auth) {
		wait(b->ensureBearerToken());
		return Void();
	}
	if (b->knobs.aliyun_auth) {
		state AliyunCredentialConfig aliyunConfig = b->aliyunCredentials;
		state double aliyunStart = timer_monotonic();
		try {
			// a plain copy for the lambda: state variables live in the actor object and cannot be captured
			AliyunCredentialConfig cfg = aliyunConfig;
			AliyunCredentials creds = wait(fetchCredentials([cfg] { return getSecretAliyun(cfg); }));
			S3BlobStoreEndpoint::Credentials fdbCreds;
			fdbCreds.key = creds.key;
			fdbCreds.secret = creds.secret;
			fdbCreds.securityToken = creds.token;
			b->credentials = fdbCreds;
			TraceEvent("S3BlobStoreGotAliyunCredentials")
			    .suppressFor(60)
			    .detail("Duration", timer_monotonic() - aliyunStart)
			    .detail("KeyIdSuffix", creds.key.size() > 4 ? creds.key.substr(creds.key.size() - 4) : creds.key)
			    .detail("CredentialProviderType", aliyunCredentialProviderTypeName(aliyunConfig.providerType))
			    .detail("CredentialSource", aliyunCredentialProviderTypeName(creds.source));
		} catch (Error& e) {
			if (e.code() == error_code_backup_auth_missing) {
				TraceEvent(SevWarn, "S3BlobStoreAliyunCredsEmpty")
				    .detail("CredentialProviderType", aliyunCredentialProviderTypeName(aliyunConfig.providerType));
				fprintf(stderr,
				        "ERROR: The Alibaba Cloud SDK returned no credentials for aliyun_auth (provider %s). This "
				        "needs an ECS instance RAM role, ACK RRSA variables, or ALIBABA_CLOUD_ACCESS_KEY_ID and "
				        "ALIBABA_CLOUD_ACCESS_KEY_SECRET in the environment of every fdbbackup and backup_agent.\n",
				        aliyunCredentialProviderTypeName(aliyunConfig.providerType));
			}
			throw;
		}
		return Void();
	}
	if (b->knobs.sdk_auth) {
		state AwsCredentialConfig awsConfig = b->awsCredentials;
		state double sdkStart = timer_monotonic();
		try {
			// a plain copy for the lambda: state variables live in the actor object and cannot be captured
			AwsCredentialConfig cfg = awsConfig;
			S3BlobStoreEndpoint::Credentials creds = wait(fetchCredentials([cfg] { return getSecretSdk(cfg); }));
			b->credentials = creds;
			TraceEvent("S3BlobStoreGotSdkCredentials")
			    .suppressFor(60)
			    .detail("Duration", timer_monotonic() - sdkStart)
			    .detail("KeyIdSuffix", creds.key.size() > 4 ? creds.key.substr(creds.key.size() - 4) : creds.key)
			    .detail("CredentialsProviderType", awsCredentialProviderTypeName(awsConfig.providerType))
			    .detail("RoleArn", awsConfig.roleArn);
		} catch (Error& e) {
			if (e.code() == error_code_backup_auth_missing) {
				TraceEvent(SevWarn, "S3BlobStoreAWSCredsEmpty")
				    .detail("CredentialsProviderType", awsCredentialProviderTypeName(awsConfig.providerType))
				    .detail("RoleArn", awsConfig.roleArn);
				fprintf(stderr,
				        "ERROR: The AWS SDK returned no credentials for sdk_auth (provider %s%s%s). This needs "
				        "credentials in the environment, a profile, an EC2 instance role, an ECS task role or a web "
				        "identity; with role_arn, the base identity also needs sts:AssumeRole on that role.\n",
				        awsCredentialProviderTypeName(awsConfig.providerType),
				        awsConfig.roleArn.empty() ? "" : ", assuming ",
				        awsConfig.roleArn.c_str());
			}
			throw;
		}
		return Void();
	}
	std::vector<std::string>* pFiles = (std::vector<std::string>*)g_network->global(INetwork::enBlobCredentialFiles);
	if (pFiles == nullptr)
		return Void();

	if (!b->credentials.present()) {
		return Void();
	}

	state std::vector<Future<Optional<json_spirit::mObject>>> reads;
	for (auto& f : *pFiles)
		reads.push_back(tryReadJSONFile(f));

	wait(waitForAll(reads));

	std::string accessKey = b->lookupKey ? "" : b->credentials.get().key;
	std::string credentialsFileKey = accessKey + "@" + b->host;

	int invalid = 0;

	for (auto& f : reads) {
		// If value not present then the credentials file wasn't readable or valid.  Continue to check other results.
		if (!f.get().present()) {
			++invalid;
			continue;
		}

		JSONDoc doc(f.get().get());
		if (doc.has("accounts") && doc.last().type() == json_spirit::obj_type) {
			JSONDoc accounts(doc.last().get_obj());
			if (accounts.has(credentialsFileKey, false) && accounts.last().type() == json_spirit::obj_type) {
				JSONDoc account(accounts.last());
				S3BlobStoreEndpoint::Credentials creds = b->credentials.get();
				if (b->lookupKey) {
					std::string apiKey;
					if (account.tryGet("api_key", apiKey))
						creds.key = apiKey;
					else
						continue;
				}
				if (b->lookupSecret) {
					std::string secret;
					if (account.tryGet("secret", secret))
						creds.secret = secret;
					else
						continue;
				}
				std::string token;
				if (account.tryGet("token", token))
					creds.securityToken = token;
				b->credentials = creds;
				return Void();
			}
		}
	}

	// If any sources were invalid
	if (invalid > 0)
		throw backup_auth_unreadable();

	// All sources were valid but didn't contain the desired info
	throw backup_auth_missing();
}

Future<Void> S3BlobStoreEndpoint::updateSecret() {
	return updateSecret_impl(Reference<S3BlobStoreEndpoint>::addRef(this));
}

ACTOR Future<S3BlobStoreEndpoint::ReusableConnection> connect_impl(Reference<S3BlobStoreEndpoint> b,
                                                                   bool* reusingConn) {
	// First try to get a connection from the pool
	*reusingConn = false;
	while (!b->connectionPool->pool.empty()) {
		S3BlobStoreEndpoint::ReusableConnection rconn = b->connectionPool->pool.front();
		b->connectionPool->pool.pop();

		// If the connection expires in the future then return it
		if (rconn.expirationTime > now()) {
			*reusingConn = true;
			++b->blobStats->reusedConnections;
			TraceEvent("S3BlobStoreEndpointReusingConnected")
			    .suppressFor(60)
			    .detail("RemoteEndpoint", rconn.conn->getPeerAddress())
			    .detail("ExpiresIn", rconn.expirationTime - now())
			    .detail("Proxy", b->proxyHost.orDefault(""));
			return rconn;
		}
		++b->blobStats->expiredConnections;
	}
	++b->blobStats->newConnections;
	std::string host = b->host, service = b->service;
	TraceEvent(SevDebug, "S3BlobStoreEndpointBuildingNewConnection")
	    .detail("UseProxy", b->useProxy)
	    .detail("TLS", b->knobs.secure_connection == 1)
	    .detail("Host", host)
	    .detail("Service", service)
	    .log();
	if (service.empty()) {
		if (b->useProxy) {
			fprintf(stderr, "ERROR: Port can't be empty when using HTTP proxy.\n");
			throw connection_failed();
		}
		service = b->knobs.secure_connection ? "https" : "http";
	}
	bool isTLS = b->knobs.isTLS();
	state Reference<IConnection> conn;
	if (b->useProxy) {
		if (isTLS) {
			Reference<IConnection> _conn =
			    wait(HTTP::proxyConnect(host, service, b->proxyHost.get(), b->proxyPort.get()));
			conn = _conn;
		} else {
			host = b->proxyHost.get();
			service = b->proxyPort.get();
			Reference<IConnection> _conn = wait(INetworkConnections::net()->connect(host, service, false));
			conn = _conn;
		}
	} else {
		wait(store(conn, INetworkConnections::net()->connect(host, service, isTLS)));
	}
	wait(conn->connectHandshake());

	TraceEvent("S3BlobStoreEndpointNewConnectionSuccess")
	    .suppressFor(60)
	    .detail("RemoteEndpoint", conn->getPeerAddress())
	    .detail("ExpiresIn", b->knobs.max_connection_life)
	    .detail("Proxy", b->proxyHost.orDefault(""));

	if (b->lookupKey || b->lookupSecret || b->knobs.sdk_auth || b->knobs.gcp_auth || b->knobs.aliyun_auth)
		wait(b->updateSecret());

	return S3BlobStoreEndpoint::ReusableConnection({ conn, now() + b->knobs.max_connection_life });
}

Future<S3BlobStoreEndpoint::ReusableConnection> S3BlobStoreEndpoint::connect(bool* reusing) {
	return connect_impl(Reference<S3BlobStoreEndpoint>::addRef(this), reusing);
}

void S3BlobStoreEndpoint::returnConnection(ReusableConnection& rconn) {
	// If it expires in the future then add it to the pool in the front
	if (rconn.expirationTime > now()) {
		connectionPool->pool.push(rconn);
	} else {
		++blobStats->expiredConnections;
	}
	rconn.conn = Reference<IConnection>();
}

std::string awsCanonicalURI(const std::string& resource, std::vector<std::string>& queryParameters, bool isV4) {
	StringRef resourceRef(resource);
	resourceRef.eat("/");
	std::string canonicalURI("/" + resourceRef.toString());
	size_t q = canonicalURI.find_last_of('?');
	if (q != canonicalURI.npos)
		canonicalURI.resize(q);
	if (isV4) {
		canonicalURI = HTTP::awsV4URIEncode(canonicalURI, false);
	} else {
		canonicalURI = HTTP::urlEncode(canonicalURI);
	}

	// Create the canonical query string
	std::string queryString;
	q = resource.find_last_of('?');
	if (q != queryString.npos)
		queryString = resource.substr(q + 1);

	StringRef qStr(queryString);
	StringRef queryParameter;
	while ((queryParameter = qStr.eat("&")) != StringRef()) {
		StringRef param = queryParameter.eat("=");
		StringRef value = queryParameter.eat();

		if (isV4) {
			queryParameters.push_back(HTTP::awsV4URIEncode(param.toString(), true) + "=" +
			                          HTTP::awsV4URIEncode(value.toString(), true));
		} else {
			queryParameters.push_back(HTTP::urlEncode(param.toString()) + "=" + HTTP::urlEncode(value.toString()));
		}
	}

	return canonicalURI;
}

// Do a request, get a Response.
// Request content is provided as UnsentPacketQueue *pContent which will be depleted as bytes are sent but the queue
// itself must live for the life of this actor and be destroyed by the caller
ACTOR Future<Reference<HTTP::IncomingResponse>> doRequest_impl(Reference<S3BlobStoreEndpoint> bstore,
                                                               std::string verb,
                                                               std::string resource,
                                                               HTTP::Headers headers,
                                                               UnsentPacketQueue* pContent,
                                                               int contentLen,
                                                               std::set<unsigned int> successCodes) {
	state UnsentPacketQueue contentCopy;
	state Reference<HTTP::OutgoingRequest> req = makeReference<HTTP::OutgoingRequest>();
	req->verb = verb;
	req->data.content = &contentCopy;
	req->data.contentLen = contentLen;

	req->data.headers = headers;
	req->data.headers["Host"] = bstore->host;
	req->data.headers["Accept"] = "application/xml";

	// Avoid to send request with an empty resouce.
	if (resource.empty()) {
		resource = "/";
	}

	// Merge extraHeaders into headers
	for (const auto& [k, v] : bstore->extraHeaders) {
		std::string& fieldValue = req->data.headers[k];
		if (!fieldValue.empty()) {
			fieldValue.append(",");
		}
		fieldValue.append(v);
	}

	// For requests with content to upload, the request timeout should be at least twice the amount of time
	// it would take to upload the content given the upload bandwidth and concurrency limits.
	int bandwidthThisRequest = 1 + bstore->knobs.max_send_bytes_per_second / bstore->knobs.concurrent_uploads;
	int contentUploadSeconds = contentLen / bandwidthThisRequest;
	state int requestTimeout = std::max(bstore->knobs.request_timeout_min, 3 * contentUploadSeconds);

	wait(bstore->concurrentRequests.take());
	state FlowLock::Releaser globalReleaser(bstore->concurrentRequests, 1);

	state int maxTries = std::min(bstore->knobs.request_tries, bstore->knobs.connect_tries);
	state int thisTry = 1;
	state double nextRetryDelay = 2.0;

	loop {
		state Optional<Error> err;
		state Optional<NetworkAddress> remoteAddress;
		state bool connectionEstablished = false;
		state Reference<HTTP::IncomingResponse> r;
		state std::string canonicalURI = resource;
		state UID connID = UID();
		state double reqStartTimer;
		state double connectStartTimer = g_network->timer();
		state bool reusingConn = false;
		state bool fastRetry = false;

		try {
			// Start connecting
			Future<S3BlobStoreEndpoint::ReusableConnection> frconn = bstore->connect(&reusingConn);

			// Make a shallow copy of the queue by calling addref() on each buffer in the chain and then prepending that
			// chain to contentCopy
			req->data.content->discardAll();
			if (pContent != nullptr) {
				PacketBuffer* pFirst = pContent->getUnsent();
				PacketBuffer* pLast = nullptr;
				for (PacketBuffer* p = pFirst; p != nullptr; p = p->nextPacketBuffer()) {
					p->addref();
					// Also reset the sent count on each buffer
					p->bytes_sent = 0;
					pLast = p;
				}
				req->data.content->prependWriteBuffer(pFirst, pLast);
			}

			// Finish connecting, do request
			state S3BlobStoreEndpoint::ReusableConnection rconn =
			    wait(timeoutError(frconn, bstore->knobs.connect_timeout));
			connectionEstablished = true;
			connID = rconn.conn->getDebugID();

			// A pooled connection can outlive the bearer token, so check the token before every request.
			if (bstore->knobs.gcp_auth) {
				wait(bstore->ensureBearerToken());
			}
			reqStartTimer = g_network->timer();

			// Finish/update the request headers (which includes Date header)
			// This must be done AFTER the connection is ready because if credentials are coming from disk they are
			// refreshed when a new connection is established and setAuthHeaders() would need the updated secret.
			if (bstore->knobs.gcp_auth) {
				bstore->setBearerAuthHeaders(req->data.headers);
			} else {
				if (bstore->credentials.present() && !bstore->credentials.get().securityToken.empty())
					req->data.headers["x-amz-security-token"] = bstore->credentials.get().securityToken;
				if (CLIENT_KNOBS->HTTP_REQUEST_AWS_V4_HEADER) {
					bstore->setV4AuthHeaders(verb, resource, req->data.headers);
				} else {
					bstore->setAuthHeaders(verb, resource, req->data.headers);
				}
			}

			std::vector<std::string> queryParameters;
			canonicalURI = awsCanonicalURI(resource, queryParameters, CLIENT_KNOBS->HTTP_REQUEST_AWS_V4_HEADER);
			if (!queryParameters.empty()) {
				canonicalURI += "?";
				canonicalURI += boost::algorithm::join(queryParameters, "&");
			}

			if (bstore->useProxy && bstore->knobs.secure_connection == 0) {
				// Has to be in absolute-form.
				canonicalURI = "http://" + bstore->host + ":" + bstore->service + canonicalURI;
			}

			req->resource = canonicalURI;

			remoteAddress = rconn.conn->getPeerAddress();
			wait(bstore->requestRate->getAllowance(1));

			Future<Reference<HTTP::IncomingResponse>> reqF =
			    HTTP::doRequest(rconn.conn, req, bstore->sendRate, &bstore->s_stats.bytes_sent, bstore->recvRate);

			// if we reused a connection from the pool, and immediately got an error, retry immediately discarding the
			// connection
			if (reqF.isReady() && reusingConn) {
				fastRetry = true;
			}

			Reference<HTTP::IncomingResponse> _r = wait(timeoutError(reqF, requestTimeout));
			r = _r;

			// Since the response was parsed successfully (which is why we are here) reuse the connection unless we
			// received the "Connection: close" header.
			if (r->data.headers["Connection"] != "close") {
				bstore->returnConnection(rconn);
			} else {
				++bstore->blobStats->expiredConnections;
			}
			rconn.conn.clear();

		} catch (Error& e) {
			TraceEvent("S3BlobStoreDoRequestError").errorUnsuppressed(e);
			if (e.code() == error_code_actor_cancelled)
				throw;
			// TODO: should this also do rconn.conn.clear()? (would need to extend lifetime outside of try block)
			err = e;
		}

		double end = g_network->timer();
		double connectDuration = reqStartTimer - connectStartTimer;
		double reqDuration = end - reqStartTimer;
		bstore->blobStats->requestLatency.addMeasurement(reqDuration);

		// If err is not present then r is valid.
		// If r->code is in successCodes then record the successful request and return r.
		if (!err.present() && successCodes.count(r->code) != 0) {
			bstore->s_stats.requests_successful++;
			++bstore->blobStats->requestsSuccessful;
			return r;
		}

		// Otherwise, this request is considered failed.  Update failure count.
		bstore->s_stats.requests_failed++;
		++bstore->blobStats->requestsFailed;

		// All errors in err are potentially retryable as well as certain HTTP response codes...
		bool retryable = err.present() || r->code == 500 || r->code == 502 || r->code == 503 || r->code == 429;
		// Not being able to get credentials from the cloud SDK (metadata server, STS AssumeRole, credential files) is
		// a deployment problem that retries will not fix.
		if ((bstore->knobs.gcp_auth || bstore->knobs.sdk_auth || bstore->knobs.aliyun_auth) && err.present() &&
		    err.get().code() == error_code_backup_auth_missing)
			retryable = false;

		// But only if our previous attempt was not the last allowable try.
		retryable = retryable && (thisTry < maxTries);

		if (!retryable || !err.present()) {
			fastRetry = false;
		}

		TraceEvent event(SevWarn,
		                 retryable ? (fastRetry ? "S3BlobStoreEndpointRequestFailedFastRetryable"
		                                        : "S3BlobStoreEndpointRequestFailedRetryable")
		                           : "S3BlobStoreEndpointRequestFailed");

		bool connectionFailed = false;
		// Attach err to trace event if present, otherwise extract some stuff from the response
		if (err.present()) {
			event.errorUnsuppressed(err.get());
			if (err.get().code() == error_code_connection_failed) {
				connectionFailed = true;
			}
		}
		event.suppressFor(60);
		if (!err.present()) {
			event.detail("ResponseCode", r->code);
		}

		event.detail("ConnectionEstablished", connectionEstablished);
		event.detail("ReusingConn", reusingConn);
		if (connectionEstablished) {
			event.detail("ConnID", connID);
			event.detail("ConnectDuration", connectDuration);
			event.detail("ReqDuration", reqDuration);
		}

		if (remoteAddress.present())
			event.detail("RemoteEndpoint", remoteAddress.get());
		else
			event.detail("RemoteHost", bstore->host);

		event.detail("Verb", verb)
		    .detail("Resource", resource)
		    .detail("ThisTry", thisTry)
		    .detail("URI", canonicalURI)
		    .detail("Proxy", bstore->proxyHost.orDefault(""));

		// If r is not valid or not code 429 then increment the try count.  429's will not count against the attempt
		// limit. Also skip incrementing the retry count for fast retries
		if (!fastRetry && (!r || r->code != 429))
			++thisTry;

		if (fastRetry) {
			++bstore->blobStats->fastRetries;
			wait(delay(0));
		} else if (retryable) {
			// We will wait delay seconds before the next retry, start with nextRetryDelay.
			double delay = nextRetryDelay;
			// conenctionFailed is treated specially as we know proxy to AWS can only serve 1 request per connection
			// so there is no point of waiting too long, instead retry more aggressively
			double limit =
			    connectionFailed ? bstore->knobs.max_delay_connection_failed : bstore->knobs.max_delay_retryable_error;
			// Double but limit the *next* nextRetryDelay.
			nextRetryDelay = std::min(nextRetryDelay * 2, limit);
			// If r is valid then obey the Retry-After response header if present.
			if (r) {
				auto iRetryAfter = r->data.headers.find("Retry-After");
				if (iRetryAfter != r->data.headers.end()) {
					event.detail("RetryAfterHeader", iRetryAfter->second);
					char* pEnd;
					double retryAfter = strtod(iRetryAfter->second.c_str(), &pEnd);
					if (*pEnd) // If there were other characters then don't trust the parsed value, use a probably safe
					           // value of 5 minutes.
						retryAfter = 300;
					// Update delay
					delay = std::max(delay, retryAfter);
				}
			}

			// Log the delay then wait.

			event.detail("RetryDelay", delay);
			wait(::delay(delay));
		} else {
			// We can't retry, so throw something.

			// This error code means the authentication header was not accepted, likely the account or key is wrong.
			if (r && r->code == 406)
				throw http_not_accepted();

			if (r && r->code == 401)
				throw http_auth_failed();

			// Recognize and throw specific errors
			if (err.present()) {
				int code = err.get().code();

				// If we get a timed_out error during the the connect() phase, we'll call that connection_failed despite
				// the fact that there was technically never a 'connection' to begin with.  It differentiates between an
				// active connection timing out vs a connection timing out, though not between an active connection
				// failing vs connection attempt failing.
				// TODO:  Add more error types?
				if (code == error_code_timed_out && !connectionEstablished) {
					TraceEvent(SevWarn, "S3BlobStoreEndpointConnectTimeout")
					    .suppressFor(60)
					    .detail("Timeout", requestTimeout);
					throw connection_failed();
				}

				if (code == error_code_timed_out || code == error_code_connection_failed ||
				    code == error_code_lookup_failed)
					throw err.get();
			}

			throw http_request_failed();
		}
	}
}

Future<Reference<HTTP::IncomingResponse>> S3BlobStoreEndpoint::doRequest(std::string const& verb,
                                                                         std::string const& resource,
                                                                         const HTTP::Headers& headers,
                                                                         UnsentPacketQueue* pContent,
                                                                         int contentLen,
                                                                         std::set<unsigned int> successCodes) {
	return doRequest_impl(
	    Reference<S3BlobStoreEndpoint>::addRef(this), verb, resource, headers, pContent, contentLen, successCodes);
}

ACTOR Future<Void> listObjectsStream_impl(Reference<S3BlobStoreEndpoint> bstore,
                                          std::string bucket,
                                          PromiseStream<S3BlobStoreEndpoint::ListResult> results,
                                          Optional<std::string> prefix,
                                          Optional<char> delimiter,
                                          int maxDepth,
                                          std::function<bool(std::string const&)> recurseFilter) {
	// Request 1000 keys at a time, the maximum allowed
	state std::string resource = constructResourcePath(bstore, bucket, "");

	resource.append("/?max-keys=1000");
	if (prefix.present())
		resource.append("&prefix=").append(prefix.get());
	if (delimiter.present())
		resource.append("&delimiter=").append(std::string(1, delimiter.get()));
	resource.append("&marker=");
	state std::string lastFile;
	state bool more = true;

	state std::vector<Future<Void>> subLists;

	while (more) {
		wait(bstore->concurrentLists.take());
		state FlowLock::Releaser listReleaser(bstore->concurrentLists, 1);

		HTTP::Headers headers;
		state std::string fullResource = resource + lastFile;
		lastFile.clear();
		Reference<HTTP::IncomingResponse> r =
		    wait(bstore->doRequest("GET", fullResource, headers, nullptr, 0, { 200 }));
		listReleaser.release();

		try {
			S3BlobStoreEndpoint::ListResult listResult;
			xml_document<> doc;

			// Copy content because rapidxml will modify it during parse
			std::string content = r->data.content;
			doc.parse<0>((char*)content.c_str());

			// There should be exactly one node
			xml_node<>* result = doc.first_node();
			if (result == nullptr || strcmp(result->name(), "ListBucketResult") != 0) {
				throw http_bad_response();
			}

			xml_node<>* n = result->first_node();
			while (n != nullptr) {
				const char* name = n->name();
				if (strcmp(name, "IsTruncated") == 0) {
					const char* val = n->value();
					if (strcmp(val, "true") == 0) {
						more = true;
					} else if (strcmp(val, "false") == 0) {
						more = false;
					} else {
						throw http_bad_response();
					}
				} else if (strcmp(name, "Contents") == 0) {
					S3BlobStoreEndpoint::ObjectInfo object;

					xml_node<>* key = n->first_node("Key");
					if (key == nullptr) {
						throw http_bad_response();
					}
					object.name = key->value();

					xml_node<>* size = n->first_node("Size");
					if (size == nullptr) {
						throw http_bad_response();
					}
					object.size = strtoull(size->value(), nullptr, 10);

					listResult.objects.push_back(object);
				} else if (strcmp(name, "CommonPrefixes") == 0) {
					xml_node<>* prefixNode = n->first_node("Prefix");
					while (prefixNode != nullptr) {
						const char* prefix = prefixNode->value();
						// If recursing, queue a sub-request, otherwise add the common prefix to the result.
						if (maxDepth > 0) {
							// If there is no recurse filter or the filter returns true then start listing the subfolder
							if (!recurseFilter || recurseFilter(prefix)) {
								subLists.push_back(bstore->listObjectsStream(
								    bucket, results, prefix, delimiter, maxDepth - 1, recurseFilter));
							}
							// Since prefix will not be in the final listResult below we have to set lastFile here in
							// case it's greater than the last object
							lastFile = prefix;
						} else {
							listResult.commonPrefixes.push_back(prefix);
						}

						prefixNode = prefixNode->next_sibling("Prefix");
					}
				}

				n = n->next_sibling();
			}

			results.send(listResult);

			if (more) {
				// lastFile will be the last commonprefix for which a sublist was started, if any.
				// If there are any objects and the last one is greater than lastFile then make it the new lastFile.
				if (!listResult.objects.empty() && lastFile < listResult.objects.back().name) {
					lastFile = listResult.objects.back().name;
				}
				// If there are any common prefixes and the last one is greater than lastFile then make it the new
				// lastFile.
				if (!listResult.commonPrefixes.empty() && lastFile < listResult.commonPrefixes.back()) {
					lastFile = listResult.commonPrefixes.back();
				}

				// If lastFile is empty at this point, something has gone wrong.
				if (lastFile.empty()) {
					TraceEvent(SevWarn, "S3BlobStoreEndpointListNoNextMarker")
					    .suppressFor(60)
					    .detail("Resource", fullResource);
					throw http_bad_response();
				}
			}
		} catch (Error& e) {
			if (e.code() != error_code_actor_cancelled)
				TraceEvent(SevWarn, "S3BlobStoreEndpointListResultParseError")
				    .errorUnsuppressed(e)
				    .suppressFor(60)
				    .detail("Resource", fullResource);
			throw http_bad_response();
		}
	}

	wait(waitForAll(subLists));

	return Void();
}

Future<Void> S3BlobStoreEndpoint::listObjectsStream(std::string const& bucket,
                                                    PromiseStream<ListResult> results,
                                                    Optional<std::string> prefix,
                                                    Optional<char> delimiter,
                                                    int maxDepth,
                                                    std::function<bool(std::string const&)> recurseFilter) {
	return listObjectsStream_impl(
	    Reference<S3BlobStoreEndpoint>::addRef(this), bucket, results, prefix, delimiter, maxDepth, recurseFilter);
}

ACTOR Future<S3BlobStoreEndpoint::ListResult> listObjects_impl(Reference<S3BlobStoreEndpoint> bstore,
                                                               std::string bucket,
                                                               Optional<std::string> prefix,
                                                               Optional<char> delimiter,
                                                               int maxDepth,
                                                               std::function<bool(std::string const&)> recurseFilter) {
	state S3BlobStoreEndpoint::ListResult results;
	state PromiseStream<S3BlobStoreEndpoint::ListResult> resultStream;
	state Future<Void> done =
	    bstore->listObjectsStream(bucket, resultStream, prefix, delimiter, maxDepth, recurseFilter);
	// Wrap done in an actor which sends end_of_stream because list does not so that many lists can write to the same
	// stream
	done = map(done, [=](Void) mutable {
		resultStream.sendError(end_of_stream());
		return Void();
	});

	try {
		loop {
			choose {
				// Throw if done throws, otherwise don't stop until end_of_stream
				when(wait(done)) {
					done = Never();
				}

				when(S3BlobStoreEndpoint::ListResult info = waitNext(resultStream.getFuture())) {
					results.commonPrefixes.insert(
					    results.commonPrefixes.end(), info.commonPrefixes.begin(), info.commonPrefixes.end());
					results.objects.insert(results.objects.end(), info.objects.begin(), info.objects.end());
				}
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream)
			throw;
	}

	return results;
}

Future<S3BlobStoreEndpoint::ListResult> S3BlobStoreEndpoint::listObjects(
    std::string const& bucket,
    Optional<std::string> prefix,
    Optional<char> delimiter,
    int maxDepth,
    std::function<bool(std::string const&)> recurseFilter) {
	return listObjects_impl(
	    Reference<S3BlobStoreEndpoint>::addRef(this), bucket, prefix, delimiter, maxDepth, recurseFilter);
}

ACTOR Future<std::vector<std::string>> listBuckets_impl(Reference<S3BlobStoreEndpoint> bstore) {
	state std::string resource = "/?marker=";
	state std::string lastName;
	state bool more = true;
	state std::vector<std::string> buckets;

	while (more) {
		wait(bstore->concurrentLists.take());
		state FlowLock::Releaser listReleaser(bstore->concurrentLists, 1);

		HTTP::Headers headers;
		state std::string fullResource = resource + lastName;
		Reference<HTTP::IncomingResponse> r =
		    wait(bstore->doRequest("GET", fullResource, headers, nullptr, 0, { 200 }));
		listReleaser.release();

		try {
			xml_document<> doc;

			// Copy content because rapidxml will modify it during parse
			std::string content = r->data.content;
			doc.parse<0>((char*)content.c_str());

			// There should be exactly one node
			xml_node<>* result = doc.first_node();
			if (result == nullptr || strcmp(result->name(), "ListAllMyBucketsResult") != 0) {
				throw http_bad_response();
			}

			more = false;
			xml_node<>* truncated = result->first_node("IsTruncated");
			if (truncated != nullptr && strcmp(truncated->value(), "true") == 0) {
				more = true;
			}

			xml_node<>* bucketsNode = result->first_node("Buckets");
			if (bucketsNode != nullptr) {
				xml_node<>* bucketNode = bucketsNode->first_node("Bucket");
				while (bucketNode != nullptr) {
					xml_node<>* nameNode = bucketNode->first_node("Name");
					if (nameNode == nullptr) {
						throw http_bad_response();
					}
					const char* name = nameNode->value();
					buckets.push_back(name);

					bucketNode = bucketNode->next_sibling("Bucket");
				}
			}

			if (more) {
				lastName = buckets.back();
			}

		} catch (Error& e) {
			if (e.code() != error_code_actor_cancelled)
				TraceEvent(SevWarn, "S3BlobStoreEndpointListBucketResultParseError")
				    .errorUnsuppressed(e)
				    .suppressFor(60)
				    .detail("Resource", fullResource);
			throw http_bad_response();
		}
	}

	return buckets;
}

Future<std::vector<std::string>> S3BlobStoreEndpoint::listBuckets() {
	return listBuckets_impl(Reference<S3BlobStoreEndpoint>::addRef(this));
}

std::string S3BlobStoreEndpoint::hmac_sha1(Credentials const& creds, std::string const& msg) {
	std::string key = creds.secret;

	// Hash key to shorten it if it is longer than SHA1 block size
	if (key.size() > 64) {
		key = SHA1::from_string(key);
	}

	// Pad key up to SHA1 block size if needed
	key.append(64 - key.size(), '\0');

	std::string kipad = key;
	for (int i = 0; i < 64; ++i)
		kipad[i] ^= '\x36';

	std::string kopad = key;
	for (int i = 0; i < 64; ++i)
		kopad[i] ^= '\x5c';

	kipad.append(msg);
	std::string hkipad = SHA1::from_string(kipad);
	kopad.append(hkipad);
	return SHA1::from_string(kopad);
}

std::string sha256_hex(std::string str) {
	unsigned char hash[SHA256_DIGEST_LENGTH];
	SHA256_CTX sha256;
	SHA256_Init(&sha256);
	SHA256_Update(&sha256, str.c_str(), str.size());
	SHA256_Final(hash, &sha256);
	std::stringstream ss;
	for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
		ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
	}
	return ss.str();
}

std::string hmac_sha256_hex(std::string key, std::string msg) {
	unsigned char hash[32];

	HMAC_CTX* hmac = HMAC_CTX_new();
	HMAC_Init_ex(hmac, &key[0], key.length(), EVP_sha256(), NULL);
	HMAC_Update(hmac, (unsigned char*)&msg[0], msg.length());
	unsigned int len = 32;
	HMAC_Final(hmac, hash, &len);
	HMAC_CTX_free(hmac);

	std::stringstream ss;
	ss << std::hex << std::setfill('0');
	for (int i = 0; i < len; i++) {
		ss << std::hex << std::setw(2) << (unsigned int)hash[i];
	}
	return (ss.str());
}

std::string hmac_sha256(std::string key, std::string msg) {
	unsigned char hash[32];

	HMAC_CTX* hmac = HMAC_CTX_new();
	HMAC_Init_ex(hmac, &key[0], key.length(), EVP_sha256(), NULL);
	HMAC_Update(hmac, (unsigned char*)&msg[0], msg.length());
	unsigned int len = 32;
	HMAC_Final(hmac, hash, &len);
	HMAC_CTX_free(hmac);

	std::stringstream ss;
	ss << std::setfill('0');
	for (int i = 0; i < len; i++) {
		ss << hash[i];
	}
	return (ss.str());
}

// Date and Time parameters are used for unit testing
void S3BlobStoreEndpoint::setV4AuthHeaders(std::string const& verb,
                                           std::string const& resource,
                                           HTTP::Headers& headers,
                                           std::string date,
                                           std::string datestamp) {
	if (!credentials.present()) {
		return;
	}
	Credentials creds = credentials.get();
	// std::cout << "========== Starting===========" << std::endl;
	std::string accessKey = creds.key;
	std::string secretKey = creds.secret;
	// Create a date for headers and the credential string
	std::string amzDate;
	std::string dateStamp;
	if (date.empty() || datestamp.empty()) {
		time_t ts;
		time(&ts);
		char dateBuf[20];
		// ISO 8601 format YYYYMMDD'T'HHMMSS'Z'
		strftime(dateBuf, 20, "%Y%m%dT%H%M%SZ", gmtime(&ts));
		amzDate = dateBuf;
		strftime(dateBuf, 20, "%Y%m%d", gmtime(&ts));
		dateStamp = dateBuf;
	} else {
		amzDate = date;
		dateStamp = datestamp;
	}

	// ************* TASK 1: CREATE A CANONICAL REQUEST *************
	// Create Create canonical URI--the part of the URI from domain to query string (use '/' if no path)
	std::vector<std::string> queryParameters;
	std::string canonicalURI = awsCanonicalURI(resource, queryParameters, true);

	std::string canonicalQueryString;
	if (!queryParameters.empty()) {
		std::sort(queryParameters.begin(), queryParameters.end());
		canonicalQueryString = boost::algorithm::join(queryParameters, "&");
	}

	using namespace boost::algorithm;
	// Create the canonical headers and signed headers
	ASSERT(!headers["Host"].empty());
	// Using unsigned payload here and adding content-md5 to the signed headers. It may be better to also include sha256
	// sum for added security.
	headers["x-amz-content-sha256"] = "UNSIGNED-PAYLOAD";
	headers["x-amz-date"] = amzDate;
	std::vector<std::pair<std::string, std::string>> headersList;
	headersList.push_back({ "host", trim_copy(headers["Host"]) + "\n" });
	if (headers.find("Content-Type") != headers.end())
		headersList.push_back({ "content-type", trim_copy(headers["Content-Type"]) + "\n" });
	if (headers.find("Content-MD5") != headers.end())
		headersList.push_back({ "content-md5", trim_copy(headers["Content-MD5"]) + "\n" });
	for (auto h : headers) {
		if (StringRef(h.first).startsWith("x-amz"_sr))
			headersList.push_back({ to_lower_copy(h.first), trim_copy(h.second) + "\n" });
	}
	std::sort(headersList.begin(), headersList.end());
	std::string canonicalHeaders;
	std::string signedHeaders;
	for (auto& i : headersList) {
		canonicalHeaders += i.first + ":" + i.second;
		signedHeaders += i.first + ";";
	}
	signedHeaders.pop_back();
	std::string canonicalRequest = verb + "\n" + canonicalURI + "\n" + canonicalQueryString + "\n" + canonicalHeaders +
	                               "\n" + signedHeaders + "\n" + headers["x-amz-content-sha256"];

	// ************* TASK 2: CREATE THE STRING TO SIGN*************
	std::string algorithm = "AWS4-HMAC-SHA256";
	std::string credentialScope = dateStamp + "/" + region + "/s3/" + "aws4_request";
	std::string stringToSign =
	    algorithm + "\n" + amzDate + "\n" + credentialScope + "\n" + sha256_hex(canonicalRequest);

	// ************* TASK 3: CALCULATE THE SIGNATURE *************
	// Create the signing key using the function defined above.
	std::string signingKey =
	    hmac_sha256(hmac_sha256(hmac_sha256(hmac_sha256("AWS4" + secretKey, dateStamp), region), "s3"), "aws4_request");
	// Sign the string_to_sign using the signing_key
	std::string signature = hmac_sha256_hex(signingKey, stringToSign);
	// ************* TASK 4: ADD SIGNING INFORMATION TO THE Header *************
	std::string authorizationHeader = algorithm + " " + "Credential=" + accessKey + "/" + credentialScope + ", " +
	                                  "SignedHeaders=" + signedHeaders + ", " + "Signature=" + signature;
	headers["Authorization"] = authorizationHeader;
}

void S3BlobStoreEndpoint::setAuthHeaders(std::string const& verb, std::string const& resource, HTTP::Headers& headers) {
	if (!credentials.present()) {
		return;
	}
	Credentials creds = credentials.get();

	std::string& date = headers["Date"];

	char dateBuf[64];
	time_t ts;
	time(&ts);
	strftime(dateBuf, 64, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&ts));
	date = dateBuf;

	std::string msg;
	msg.append(verb);
	msg.append("\n");
	auto contentMD5 = headers.find("Content-MD5");
	if (contentMD5 != headers.end())
		msg.append(contentMD5->second);
	msg.append("\n");
	auto contentType = headers.find("Content-Type");
	if (contentType != headers.end())
		msg.append(contentType->second);
	msg.append("\n");
	msg.append(date);
	msg.append("\n");
	for (auto h : headers) {
		StringRef name = h.first;
		if (name.startsWith("x-amz"_sr) || name.startsWith("x-icloud"_sr)) {
			msg.append(h.first);
			msg.append(":");
			msg.append(h.second);
			msg.append("\n");
		}
	}

	msg.append(resource);
	if (verb == "GET") {
		size_t q = resource.find_last_of('?');
		if (q != resource.npos)
			msg.resize(msg.size() - (resource.size() - q));
	}

	std::string sig = base64::encoder::from_string(hmac_sha1(creds, msg));
	// base64 encoded blocks end in \n so remove it.
	sig.resize(sig.size() - 1);
	std::string auth = "AWS ";
	auth.append(creds.key);
	auth.append(":");
	auth.append(sig);
	headers["Authorization"] = auth;
}

ACTOR Future<std::string> readEntireFile_impl(Reference<S3BlobStoreEndpoint> bstore,
                                              std::string bucket,
                                              std::string object) {
	wait(bstore->requestRateRead->getAllowance(1));

	std::string resource = constructResourcePath(bstore, bucket, object);
	HTTP::Headers headers;
	Reference<HTTP::IncomingResponse> r = wait(bstore->doRequest("GET", resource, headers, nullptr, 0, { 200, 404 }));
	if (r->code == 404)
		throw file_not_found();
	return r->data.content;
}

Future<std::string> S3BlobStoreEndpoint::readEntireFile(std::string const& bucket, std::string const& object) {
	return readEntireFile_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object);
}

ACTOR Future<Void> writeEntireFileFromBuffer_impl(Reference<S3BlobStoreEndpoint> bstore,
                                                  std::string bucket,
                                                  std::string object,
                                                  UnsentPacketQueue* pContent,
                                                  int contentLen,
                                                  std::string contentMD5) {
	if (contentLen > bstore->knobs.multipart_max_part_size)
		throw file_too_large();

	wait(bstore->requestRateWrite->getAllowance(1));
	wait(bstore->concurrentUploads.take());
	state FlowLock::Releaser uploadReleaser(bstore->concurrentUploads, 1);

	std::string resource = constructResourcePath(bstore, bucket, object);
	HTTP::Headers headers;
	// Send MD5 sum for content so blobstore can verify it
	headers["Content-MD5"] = contentMD5;
	if (!CLIENT_KNOBS->BLOBSTORE_ENCRYPTION_TYPE.empty())
		headers["x-amz-server-side-encryption"] = CLIENT_KNOBS->BLOBSTORE_ENCRYPTION_TYPE;
	state Reference<HTTP::IncomingResponse> r =
	    wait(bstore->doRequest("PUT", resource, headers, pContent, contentLen, { 200 }));

	// For uploads, Blobstore returns an MD5 sum of uploaded content so check it.
	if (!HTTP::verifyMD5(&r->data, false, contentMD5))
		throw checksum_failed();

	return Void();
}

ACTOR Future<Void> writeEntireFile_impl(Reference<S3BlobStoreEndpoint> bstore,
                                        std::string bucket,
                                        std::string object,
                                        std::string content) {
	state UnsentPacketQueue packets;
	if (content.size() > bstore->knobs.multipart_max_part_size)
		throw file_too_large();

	PacketWriter pw(packets.getWriteBuffer(content.size()), nullptr, Unversioned());
	pw.serializeBytes(content);

	// Yield because we may have just had to copy several MB's into packet buffer chain and next we have to calculate an
	// MD5 sum of it.
	// TODO:  If this actor is used to send large files then combine the summing and packetization into a loop with a
	// yield() every 20k or so.
	wait(yield());

	MD5_CTX sum;
	::MD5_Init(&sum);
	::MD5_Update(&sum, content.data(), content.size());
	std::string sumBytes;
	sumBytes.resize(16);
	::MD5_Final((unsigned char*)sumBytes.data(), &sum);
	std::string contentMD5 = base64::encoder::from_string(sumBytes);
	contentMD5.resize(contentMD5.size() - 1);

	wait(writeEntireFileFromBuffer_impl(bstore, bucket, object, &packets, content.size(), contentMD5));
	return Void();
}

Future<Void> S3BlobStoreEndpoint::writeEntireFile(std::string const& bucket,
                                                  std::string const& object,
                                                  std::string const& content) {
	return writeEntireFile_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object, content);
}

Future<Void> S3BlobStoreEndpoint::writeEntireFileFromBuffer(std::string const& bucket,
                                                            std::string const& object,
                                                            UnsentPacketQueue* pContent,
                                                            int contentLen,
                                                            std::string const& contentMD5) {
	return writeEntireFileFromBuffer_impl(
	    Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object, pContent, contentLen, contentMD5);
}

ACTOR Future<int> readObject_impl(Reference<S3BlobStoreEndpoint> bstore,
                                  std::string bucket,
                                  std::string object,
                                  void* data,
                                  int length,
                                  int64_t offset) {
	if (length <= 0)
		return 0;
	wait(bstore->requestRateRead->getAllowance(1));

	std::string resource = constructResourcePath(bstore, bucket, object);
	HTTP::Headers headers;
	headers["Range"] = format("bytes=%lld-%lld", offset, offset + length - 1);
	Reference<HTTP::IncomingResponse> r =
	    wait(bstore->doRequest("GET", resource, headers, nullptr, 0, { 200, 206, 404 }));
	if (r->code == 404)
		throw file_not_found();
	if (r->data.contentLen !=
	    r->data.content.size()) // Double check that this wasn't a header-only response, probably unnecessary
		throw io_error();
	// Copy the output bytes, server could have sent more or less bytes than requested so copy at most length bytes
	memcpy(data, r->data.content.data(), std::min<int64_t>(r->data.contentLen, length));
	return r->data.contentLen;
}

Future<int> S3BlobStoreEndpoint::readObject(std::string const& bucket,
                                            std::string const& object,
                                            void* data,
                                            int length,
                                            int64_t offset) {
	return readObject_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object, data, length, offset);
}

ACTOR static Future<std::string> beginMultiPartUpload_impl(Reference<S3BlobStoreEndpoint> bstore,
                                                           std::string bucket,
                                                           std::string object) {
	wait(bstore->requestRateWrite->getAllowance(1));

	std::string resource = constructResourcePath(bstore, bucket, object);
	resource += "?uploads";
	HTTP::Headers headers;
	if (!CLIENT_KNOBS->BLOBSTORE_ENCRYPTION_TYPE.empty())
		headers["x-amz-server-side-encryption"] = CLIENT_KNOBS->BLOBSTORE_ENCRYPTION_TYPE;
	Reference<HTTP::IncomingResponse> r = wait(bstore->doRequest("POST", resource, headers, nullptr, 0, { 200 }));

	try {
		xml_document<> doc;
		// Copy content because rapidxml will modify it during parse
		std::string content = r->data.content;

		doc.parse<0>((char*)content.c_str());

		// There should be exactly one node
		xml_node<>* result = doc.first_node();
		if (result != nullptr && strcmp(result->name(), "InitiateMultipartUploadResult") == 0) {
			xml_node<>* id = result->first_node("UploadId");
			if (id != nullptr) {
				return id->value();
			}
		}
	} catch (...) {
	}
	throw http_bad_response();
}

Future<std::string> S3BlobStoreEndpoint::beginMultiPartUpload(std::string const& bucket, std::string const& object) {
	return beginMultiPartUpload_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object);
}

ACTOR Future<std::string> uploadPart_impl(Reference<S3BlobStoreEndpoint> bstore,
                                          std::string bucket,
                                          std::string object,
                                          std::string uploadID,
                                          unsigned int partNumber,
                                          UnsentPacketQueue* pContent,
                                          int contentLen,
                                          std::string contentMD5) {
	wait(bstore->requestRateWrite->getAllowance(1));
	wait(bstore->concurrentUploads.take());
	state FlowLock::Releaser uploadReleaser(bstore->concurrentUploads, 1);

	std::string resource = constructResourcePath(bstore, bucket, object);
	resource += format("?partNumber=%d&uploadId=%s", partNumber, uploadID.c_str());
	HTTP::Headers headers;
	// Send MD5 sum for content so blobstore can verify it
	headers["Content-MD5"] = contentMD5;
	state Reference<HTTP::IncomingResponse> r =
	    wait(bstore->doRequest("PUT", resource, headers, pContent, contentLen, { 200 }));
	// TODO:  In the event that the client times out just before the request completes (so the client is unaware) then
	// the next retry will see error 400.  That could be detected and handled gracefully by retrieving the etag for the
	// successful request.

	// For uploads, Blobstore returns an MD5 sum of uploaded content so check it.
	if (!HTTP::verifyMD5(&r->data, false, contentMD5))
		throw checksum_failed();

	// No etag -> bad response.
	std::string etag = r->data.headers["ETag"];
	if (etag.empty())
		throw http_bad_response();

	return etag;
}

Future<std::string> S3BlobStoreEndpoint::uploadPart(std::string const& bucket,
                                                    std::string const& object,
                                                    std::string const& uploadID,
                                                    unsigned int partNumber,
                                                    UnsentPacketQueue* pContent,
                                                    int contentLen,
                                                    std::string const& contentMD5) {
	return uploadPart_impl(Reference<S3BlobStoreEndpoint>::addRef(this),
	                       bucket,
	                       object,
	                       uploadID,
	                       partNumber,
	                       pContent,
	                       contentLen,
	                       contentMD5);
}

ACTOR Future<Void> finishMultiPartUpload_impl(Reference<S3BlobStoreEndpoint> bstore,
                                              std::string bucket,
                                              std::string object,
                                              std::string uploadID,
                                              S3BlobStoreEndpoint::MultiPartSetT parts) {
	state UnsentPacketQueue part_list; // NonCopyable state var so must be declared at top of actor
	wait(bstore->requestRateWrite->getAllowance(1));

	std::string manifest = "<CompleteMultipartUpload>";
	for (auto& p : parts)
		manifest += format("<Part><PartNumber>%d</PartNumber><ETag>%s</ETag></Part>\n", p.first, p.second.c_str());
	manifest += "</CompleteMultipartUpload>";

	std::string resource = constructResourcePath(bstore, bucket, object);
	resource += format("?uploadId=%s", uploadID.c_str());
	HTTP::Headers headers;
	PacketWriter pw(part_list.getWriteBuffer(manifest.size()), nullptr, Unversioned());
	pw.serializeBytes(manifest);
	Reference<HTTP::IncomingResponse> r =
	    wait(bstore->doRequest("POST", resource, headers, &part_list, manifest.size(), { 200 }));
	// TODO:  In the event that the client times out just before the request completes (so the client is unaware) then
	// the next retry will see error 400.  That could be detected and handled gracefully by HEAD'ing the object before
	// upload to get its (possibly nonexistent) eTag, then if an error 400 is seen then retrieve the eTag again and if
	// it has changed then consider the finish complete.
	return Void();
}

Future<Void> S3BlobStoreEndpoint::finishMultiPartUpload(std::string const& bucket,
                                                        std::string const& object,
                                                        std::string const& uploadID,
                                                        MultiPartSetT const& parts) {
	return finishMultiPartUpload_impl(Reference<S3BlobStoreEndpoint>::addRef(this), bucket, object, uploadID, parts);
}

TEST_CASE("/backup/s3/v4headers") {
	S3BlobStoreEndpoint::Credentials creds{ "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "" };
	// GET without query parameters
	{
		S3BlobStoreEndpoint s3("s3.amazonaws.com", "443", "amazonaws", "proxy", "port", creds);
		std::string verb("GET");
		std::string resource("/test.txt");
		HTTP::Headers headers;
		headers["Host"] = "s3.amazonaws.com";
		s3.setV4AuthHeaders(verb, resource, headers, "20130524T000000Z", "20130524");
		ASSERT(headers["Authorization"] ==
		       "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/amazonaws/s3/aws4_request, "
		       "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
		       "Signature=c6037f4b174f2019d02d7085a611cef8adfe1efe583e220954dc85d59cd31ba3");
		ASSERT(headers["x-amz-date"] == "20130524T000000Z");
	}

	// GET with query parameters
	{
		S3BlobStoreEndpoint s3("s3.amazonaws.com", "443", "amazonaws", "proxy", "port", creds);
		std::string verb("GET");
		std::string resource("/test/examplebucket?Action=DescribeRegions&Version=2013-10-15");
		HTTP::Headers headers;
		headers["Host"] = "s3.amazonaws.com";
		s3.setV4AuthHeaders(verb, resource, headers, "20130524T000000Z", "20130524");
		ASSERT(headers["Authorization"] ==
		       "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/amazonaws/s3/aws4_request, "
		       "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
		       "Signature=426f04e71e191fbc30096c306fe1b11ce8f026a7be374541862bbee320cce71c");
		ASSERT(headers["x-amz-date"] == "20130524T000000Z");
	}

	// POST
	{
		S3BlobStoreEndpoint s3("s3.us-west-2.amazonaws.com", "443", "us-west-2", "proxy", "port", creds);
		std::string verb("POST");
		std::string resource("/simple.json");
		HTTP::Headers headers;
		headers["Host"] = "s3.us-west-2.amazonaws.com";
		headers["Content-Type"] = "Application/x-amz-json-1.0";
		s3.setV4AuthHeaders(verb, resource, headers, "20130524T000000Z", "20130524");
		ASSERT(headers["Authorization"] ==
		       "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-west-2/s3/aws4_request, "
		       "SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date, "
		       "Signature=cf095e36bed9cd3139c2e8b3e20c296a79d8540987711bf3a0d816b19ae00314");
		ASSERT(headers["x-amz-date"] == "20130524T000000Z");
		ASSERT(headers["Host"] == "s3.us-west-2.amazonaws.com");
		ASSERT(headers["Content-Type"] == "Application/x-amz-json-1.0");
	}

	return Void();
}

TEST_CASE("/backup/s3/gcp_auth_url") {
	// state: the try/catch below is compiled into a separate continuation by the actor compiler
	state std::string err;
	// non-knob parameters such as bucket are handed back to the caller through this map
	state S3BlobStoreEndpoint::ParametersT ignored;

	// gcp_auth needs neither credentials nor a region and survives a URL round trip
	state Reference<S3BlobStoreEndpoint> b = S3BlobStoreEndpoint::fromString(
	    "blobstore://storage.googleapis.com/fdb?bucket=b&gcp_auth=1", {}, nullptr, &err, &ignored);
	ASSERT(b->knobs.gcp_auth == 1);
	ASSERT(!b->credentials.present());
	ASSERT(ignored["bucket"] == "b");
	ASSERT(b->getResourceURL("fdb", "bucket=b").find("ga=1") != std::string::npos);

	HTTP::Headers headers;
	b->bearerToken = "tok";
	b->setBearerAuthHeaders(headers);
	ASSERT(headers["Authorization"] == "Bearer tok");
	ASSERT(!headers["Date"].empty());

	// credentials in the URL contradict gcp_auth
	try {
		S3BlobStoreEndpoint::fromString(
		    "blobstore://k:s@storage.googleapis.com/fdb?bucket=b&ga=1", {}, nullptr, &err, &ignored);
		ASSERT(false);
	} catch (Error& e) {
		ASSERT(e.code() == error_code_backup_invalid_url);
		ASSERT(err.find("remove the credentials") != std::string::npos);
	}

	// the defaults: application default credentials, no impersonation, nothing extra in the URL
	ASSERT(b->gcpCredentials.providerType == GcpCredentialProviderType::DEFAULT);
	ASSERT(b->gcpCredentials.impersonationServiceAccount.empty());
	ASSERT(b->getResourceURL("fdb", "bucket=b").find("gcp_") == std::string::npos);

	// the credential settings imply gcp_auth, are case-insensitive and survive a URL round trip
	state Reference<S3BlobStoreEndpoint> c = S3BlobStoreEndpoint::fromString(
	    "blobstore://storage.googleapis.com/fdb?bucket=b&gcp_credential_provider_type=compute_engine"
	    "&gcp_impersonation_service_account=data-access@proj.iam.gserviceaccount.com",
	    {},
	    nullptr,
	    &err,
	    &ignored);
	ASSERT(c->knobs.gcp_auth == 1);
	ASSERT(c->gcpCredentials.providerType == GcpCredentialProviderType::COMPUTE_ENGINE);
	ASSERT(c->gcpCredentials.impersonationServiceAccount == "data-access@proj.iam.gserviceaccount.com");
	state std::string roundTrip = c->getResourceURL("fdb", "bucket=b");
	ASSERT(roundTrip.find("ga=1") != std::string::npos);
	ASSERT(roundTrip.find("gcp_credential_provider_type=COMPUTE_ENGINE") != std::string::npos);
	ASSERT(roundTrip.find("gcp_impersonation_service_account=data-access@proj.iam.gserviceaccount.com") !=
	       std::string::npos);
	state Reference<S3BlobStoreEndpoint> d = S3BlobStoreEndpoint::fromString(roundTrip, {}, nullptr, &err, &ignored);
	ASSERT(d->gcpCredentials.providerType == GcpCredentialProviderType::COMPUTE_ENGINE);
	ASSERT(d->gcpCredentials.impersonationServiceAccount == c->gcpCredentials.impersonationServiceAccount);

	// the '@' of the email may also be URL-encoded, and credentials before the host still parse when a parameter
	// value contains an '@'
	state Reference<S3BlobStoreEndpoint> e = S3BlobStoreEndpoint::fromString(
	    "blobstore://storage.googleapis.com/fdb?bucket=b&ga=1&gcp_impersonation_service_account=data-access%40proj.iam"
	    ".gserviceaccount.com",
	    {},
	    nullptr,
	    &err,
	    &ignored);
	ASSERT(e->gcpCredentials.impersonationServiceAccount == "data-access@proj.iam.gserviceaccount.com");
	state Reference<S3BlobStoreEndpoint> f = S3BlobStoreEndpoint::fromString(
	    "blobstore://key:secret/with/slashes@s3.us-west-2.amazonaws.com/fdb?bucket=b&header=x-note:me@example.com",
	    {},
	    nullptr,
	    &err,
	    &ignored);
	ASSERT(f->credentials.present() && f->credentials.get().key == "key" &&
	       f->credentials.get().secret == "secret/with/slashes");
	ASSERT(f->host == "s3.us-west-2.amazonaws.com");
	ASSERT(f->extraHeaders["x-note"] == "me@example.com");

	// unknown provider types and malformed service account emails are rejected
	try {
		S3BlobStoreEndpoint::fromString(
		    "blobstore://storage.googleapis.com/fdb?bucket=b&gcp_credential_provider_type=vm",
		    {},
		    nullptr,
		    &err,
		    &ignored);
		ASSERT(false);
	} catch (Error& e) {
		ASSERT(e.code() == error_code_backup_invalid_url);
		ASSERT(err.find("gcp_credential_provider_type") != std::string::npos);
	}
	try {
		S3BlobStoreEndpoint::fromString(
		    "blobstore://storage.googleapis.com/fdb?bucket=b&ga=1&gcp_impersonation_service_account=not-an-email",
		    {},
		    nullptr,
		    &err,
		    &ignored);
		ASSERT(false);
	} catch (Error& e) {
		ASSERT(e.code() == error_code_backup_invalid_url);
		ASSERT(err.find("gcp_impersonation_service_account") != std::string::npos);
	}
	ASSERT(isValidGcpServiceAccountEmail("a@b.iam.gserviceaccount.com"));
	ASSERT(isValidGcpServiceAccountEmail("123-compute@developer.gserviceaccount.com"));
	ASSERT(!isValidGcpServiceAccountEmail("a@b.example.com"));
	ASSERT(!isValidGcpServiceAccountEmail("@b.iam.gserviceaccount.com"));
	ASSERT(!isValidGcpServiceAccountEmail("a b@c.iam.gserviceaccount.com"));
	return Void();
}

TEST_CASE("/backup/s3/aws_role_url") {
	state std::string err;
	state S3BlobStoreEndpoint::ParametersT ignored;

	// role_arn implies sdk_auth, the settings survive a URL round trip, and the region reaches the STS config
	state Reference<S3BlobStoreEndpoint> b = S3BlobStoreEndpoint::fromString(
	    "blobstore://s3.us-west-2.amazonaws.com/fdb?bucket=b&role_arn=arn:aws:iam::123456789012:role/data-access"
	    "&external_id=ext-1&credentials_provider_type=instance_profile",
	    {},
	    nullptr,
	    &err,
	    &ignored);
	ASSERT(b->knobs.sdk_auth == 1);
	ASSERT(!b->credentials.present());
	ASSERT(b->awsCredentials.roleArn == "arn:aws:iam::123456789012:role/data-access");
	ASSERT(b->awsCredentials.externalId == "ext-1");
	ASSERT(b->awsCredentials.providerType == AwsCredentialProviderType::INSTANCE_PROFILE);
	ASSERT(b->awsCredentials.region == "us-west-2");
	state std::string roundTrip = b->getResourceURL("fdb", "bucket=b");
	ASSERT(roundTrip.find("sa=1") != std::string::npos);
	ASSERT(roundTrip.find("role_arn=arn:aws:iam::123456789012:role/data-access") != std::string::npos);
	ASSERT(roundTrip.find("external_id=ext-1") != std::string::npos);
	ASSERT(roundTrip.find("credentials_provider_type=INSTANCE_PROFILE") != std::string::npos);
	state Reference<S3BlobStoreEndpoint> c = S3BlobStoreEndpoint::fromString(roundTrip, {}, nullptr, &err, &ignored);
	ASSERT(c->awsCredentials.roleArn == b->awsCredentials.roleArn);
	ASSERT(c->awsCredentials.externalId == b->awsCredentials.externalId);
	ASSERT(c->awsCredentials.providerType == b->awsCredentials.providerType);

	// plain sdk_auth keeps the defaults and adds nothing to the URL, and credentials the SDK resolved later are not
	// printed into it either
	state Reference<S3BlobStoreEndpoint> d = S3BlobStoreEndpoint::fromString(
	    "blobstore://s3.us-west-2.amazonaws.com/fdb?bucket=b&sa=1", {}, nullptr, &err, &ignored);
	ASSERT(d->awsCredentials.roleArn.empty() && d->awsCredentials.externalId.empty());
	ASSERT(d->awsCredentials.providerType == AwsCredentialProviderType::DEFAULT);
	ASSERT(d->getResourceURL("fdb", "bucket=b").find("role_arn") == std::string::npos);
	ASSERT(d->getResourceURL("fdb", "bucket=b").find("credentials_provider_type") == std::string::npos);
	d->credentials = S3BlobStoreEndpoint::Credentials{ "ASIAKEY", "secret", "token" };
	ASSERT(d->getResourceURL("fdb", "bucket=b").find("ASIAKEY") == std::string::npos);
	ASSERT(d->getResourceURL("fdb", "bucket=b").find("@") == std::string::npos);

	// explicit credentials are printed once, with the token when there is one
	state Reference<S3BlobStoreEndpoint> g = S3BlobStoreEndpoint::fromString(
	    "blobstore://key:secret:tok@s3.us-west-2.amazonaws.com/fdb?bucket=b", {}, nullptr, &err, &ignored);
	ASSERT(g->getResourceURL("fdb", "bucket=b").find("blobstore://key:secret:tok@s3.us-west-2.amazonaws.com/fdb") == 0);
	state Reference<S3BlobStoreEndpoint> h = S3BlobStoreEndpoint::fromString(
	    "blobstore://key:secret@s3.us-west-2.amazonaws.com/fdb?bucket=b", {}, nullptr, &err, &ignored);
	ASSERT(h->getResourceURL("fdb", "bucket=b").find("blobstore://key:secret@s3.us-west-2.amazonaws.com/fdb") == 0);

	// rejected: external_id alone, access keys together with a role, a role together with gcp_auth, a bad
	// provider type, a role that is not an ARN
	state std::vector<std::string> bad = {
		"blobstore://s3.us-west-2.amazonaws.com/fdb?bucket=b&external_id=ext-1",
		"blobstore://k:s@s3.us-west-2.amazonaws.com/fdb?bucket=b&role_arn=arn:aws:iam::1:role/r",
		"blobstore://storage.googleapis.com/fdb?bucket=b&ga=1&role_arn=arn:aws:iam::1:role/r",
		"blobstore://s3.us-west-2.amazonaws.com/fdb?bucket=b&credentials_provider_type=iam",
		"blobstore://s3.us-west-2.amazonaws.com/fdb?bucket=b&role_arn=data-access"
	};
	state int i = 0;
	for (; i < bad.size(); ++i) {
		try {
			S3BlobStoreEndpoint::fromString(bad[i], {}, nullptr, &err, &ignored);
			ASSERT(false);
		} catch (Error& e) {
			ASSERT(e.code() == error_code_backup_invalid_url);
		}
	}
	return Void();
}

TEST_CASE("/backup/s3/aliyun_auth_url") {
	state std::string err;
	state S3BlobStoreEndpoint::ParametersT ignored;

	// aliyun_auth signs with SDK-resolved keys, so the region is still required and survives a URL round trip
	state Reference<S3BlobStoreEndpoint> b = S3BlobStoreEndpoint::fromString(
	    "blobstore://b.oss-cn-beijing.aliyuncs.com/fdb?bucket=b&region=oss-cn-beijing&aliyun_auth=1",
	    {},
	    nullptr,
	    &err,
	    &ignored);
	ASSERT(b->knobs.aliyun_auth == 1);
	ASSERT(!b->credentials.present());
	ASSERT(b->aliyunCredentials.providerType == AliyunCredentialProviderType::DEFAULT);
	ASSERT(b->getResourceURL("fdb", "bucket=b").find("aa=1") != std::string::npos);
	ASSERT(b->getResourceURL("fdb", "bucket=b").find("aliyun_credential_provider_type") == std::string::npos);

	// the provider type implies aliyun_auth, is case-insensitive and is printed back
	state Reference<S3BlobStoreEndpoint> c =
	    S3BlobStoreEndpoint::fromString("blobstore://b.oss-cn-beijing.aliyuncs.com/fdb?bucket=b&region=oss-cn-beijing"
	                                    "&aliyun_credential_provider_type=ecs_ram_role",
	                                    {},
	                                    nullptr,
	                                    &err,
	                                    &ignored);
	ASSERT(c->knobs.aliyun_auth == 1);
	ASSERT(c->aliyunCredentials.providerType == AliyunCredentialProviderType::ECS_RAM_ROLE);
	state std::string roundTrip = c->getResourceURL("fdb", "bucket=b");
	ASSERT(roundTrip.find("aliyun_credential_provider_type=ECS_RAM_ROLE") != std::string::npos);
	state Reference<S3BlobStoreEndpoint> d = S3BlobStoreEndpoint::fromString(roundTrip, {}, nullptr, &err, &ignored);
	ASSERT(d->aliyunCredentials.providerType == AliyunCredentialProviderType::ECS_RAM_ROLE);
	// credentials the SDK resolved later are not printed into the URL
	d->credentials = S3BlobStoreEndpoint::Credentials{ "STS.KEY", "secret", "token" };
	ASSERT(d->getResourceURL("fdb", "bucket=b").find("@") == std::string::npos);

	// rejected: access keys together with aliyun_auth, a bad provider type, mixing with gcp_auth or sdk_auth
	state std::vector<std::string> bad = {
		"blobstore://k:s@b.oss-cn-beijing.aliyuncs.com/fdb?bucket=b&region=oss-cn-beijing&aliyun_auth=1",
		"blobstore://b.oss-cn-beijing.aliyuncs.com/"
		"fdb?bucket=b&region=oss-cn-beijing&aliyun_credential_provider_type=ram",
		"blobstore://b.oss-cn-beijing.aliyuncs.com/fdb?bucket=b&region=oss-cn-beijing&aliyun_auth=1&gcp_auth=1",
		"blobstore://b.oss-cn-beijing.aliyuncs.com/fdb?bucket=b&region=oss-cn-beijing&aliyun_auth=1&sa=1",
		"blobstore://b.oss-cn-beijing.aliyuncs.com/"
		"fdb?bucket=b&region=oss-cn-beijing&aliyun_auth=1&role_arn=arn:aws:iam::1:role/r"
	};
	state int i = 0;
	for (; i < bad.size(); ++i) {
		try {
			S3BlobStoreEndpoint::fromString(bad[i], {}, nullptr, &err, &ignored);
			ASSERT(false);
		} catch (Error& e) {
			ASSERT(e.code() == error_code_backup_invalid_url);
		}
	}
	return Void();
}

TEST_CASE("/backup/s3/guess_region") {
	std::string url = "blobstore://s3.us-west-2.amazonaws.com/resource_name?bucket=bucket_name&sa=1";

	std::string resource;
	std::string error;
	S3BlobStoreEndpoint::ParametersT parameters;
	Reference<S3BlobStoreEndpoint> s3 = S3BlobStoreEndpoint::fromString(url, {}, &resource, &error, &parameters);
	ASSERT(s3->getRegion() == "us-west-2");

	url = "blobstore://s3.us-west-2.amazonaws.com/resource_name?bucket=bucket_name&sc=922337203685477580700";
	try {
		s3 = S3BlobStoreEndpoint::fromString(url, {}, &resource, &error, &parameters);
		ASSERT(false); // not reached
	} catch (Error& e) {
		// conversion of 922337203685477580700 to long int will overflow
		ASSERT_EQ(e.code(), error_code_backup_invalid_url);
	}
	return Void();
}
