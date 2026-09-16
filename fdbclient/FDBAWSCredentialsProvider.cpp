/*
 * FDBAWSCredentialsProvider.cpp
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

#include "fdbclient/FDBAWSCredentialsProvider.h"

#ifdef WITH_AWS_BACKUP

#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "aws/core/auth/AWSCredentialsProvider.h"
#include "aws/core/auth/STSCredentialsProvider.h"
#include "aws/core/client/ClientConfiguration.h"
#include "aws/core/client/DefaultRetryStrategy.h"
#include "aws/core/platform/Environment.h"
#include "aws/core/utils/logging/LogLevel.h"
#include "aws/identity-management/auth/STSAssumeRoleCredentialsProvider.h"
#include "aws/sts/STSClient.h"

#include "fdbclient/SystemCaBundle.h"

namespace {

// FDB_AWS_SDK_LOG_LEVEL=trace|debug|info|warn|error writes the SDK's own log (aws_sdk_<date>.log in the working
// directory), for diagnosing credential, STS and proxy problems.
Aws::Utils::Logging::LogLevel sdkLogLevel() {
	const char* level = getenv("FDB_AWS_SDK_LOG_LEVEL");
	std::string l = level == nullptr ? "" : level;
	for (char& c : l) {
		c = tolower(static_cast<unsigned char>(c));
	}
	if (l == "trace")
		return Aws::Utils::Logging::LogLevel::Trace;
	if (l == "debug")
		return Aws::Utils::Logging::LogLevel::Debug;
	if (l == "info")
		return Aws::Utils::Logging::LogLevel::Info;
	if (l == "warn")
		return Aws::Utils::Logging::LogLevel::Warn;
	if (l == "error")
		return Aws::Utils::Logging::LogLevel::Error;
	return Aws::Utils::Logging::LogLevel::Off;
}

void initSdkOnce() {
	static std::once_flag once;
	std::call_once(once, [] {
		static Aws::SDKOptions options;
		options.loggingOptions.logLevel = sdkLogLevel();
		Aws::InitAPI(options);
	});
}

std::shared_ptr<Aws::Auth::AWSCredentialsProvider> makeBaseProvider(AwsCredentialProviderType type) {
	switch (type) {
	case AwsCredentialProviderType::ENV:
		return std::make_shared<Aws::Auth::EnvironmentAWSCredentialsProvider>();
	case AwsCredentialProviderType::SYSTEM_PROPERTIES:
		return std::make_shared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>();
	case AwsCredentialProviderType::WEB_IDENTITY:
		return std::make_shared<Aws::Auth::STSAssumeRoleWebIdentityCredentialsProvider>();
	case AwsCredentialProviderType::CONTAINER:
		return std::make_shared<Aws::Auth::TaskRoleCredentialsProvider>(
		    Aws::Environment::GetEnv("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI").c_str());
	case AwsCredentialProviderType::INSTANCE_PROFILE:
		return std::make_shared<Aws::Auth::InstanceProfileCredentialsProvider>();
	case AwsCredentialProviderType::DEFAULT:
	default:
		return std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>();
	}
}

// Configuration of the STS client: the blobstore's region selects the regional STS endpoint, TLS trusts the
// system CA bundle, and the https proxy of the environment is honoured because the SDK does not read it itself.
Aws::Client::ClientConfiguration stsClientConfiguration(const AwsCredentialConfig& config) {
	Aws::Client::ClientConfiguration clientConfiguration;
	if (!config.region.empty()) {
		clientConfiguration.region = config.region.c_str();
	}
	// The SDK's 1 second connect timeout covers the TLS handshake too and is too short through a proxy or across
	// regions; the retries are bounded so that a misconfigured role fails within a minute or two.
	clientConfiguration.connectTimeoutMs = 10000;
	clientConfiguration.requestTimeoutMs = 30000;
	clientConfiguration.retryStrategy = std::make_shared<Aws::Client::DefaultRetryStrategy>(3);
	std::string caBundle = systemCaBundle();
	if (!caBundle.empty()) {
		clientConfiguration.caFile = caBundle.c_str();
	}
	const char* proxy = getenv("https_proxy");
	if (proxy == nullptr || *proxy == '\0') {
		proxy = getenv("HTTPS_PROXY");
	}
	if (proxy != nullptr && *proxy != '\0') {
		std::string p = proxy;
		size_t scheme = p.find("://");
		if (scheme != std::string::npos) {
			p = p.substr(scheme + 3);
		}
		while (!p.empty() && p.back() == '/') {
			p.pop_back();
		}
		size_t colon = p.rfind(':');
		clientConfiguration.proxyScheme = Aws::Http::Scheme::HTTP;
		clientConfiguration.proxyHost = (colon == std::string::npos ? p : p.substr(0, colon)).c_str();
		clientConfiguration.proxyPort = colon == std::string::npos ? 80 : atoi(p.c_str() + colon + 1);
	}
	return clientConfiguration;
}

std::shared_ptr<Aws::Auth::AWSCredentialsProvider> providerFor(const AwsCredentialConfig& config) {
	static std::mutex mutex;
	static std::map<std::string, std::shared_ptr<Aws::Auth::AWSCredentialsProvider>> providers;
	std::lock_guard<std::mutex> lock(mutex);
	std::string key = std::string(awsCredentialProviderTypeName(config.providerType)) + "|" + config.roleArn + "|" +
	                  config.externalId + "|" + config.region;
	std::shared_ptr<Aws::Auth::AWSCredentialsProvider>& provider = providers[key];
	if (!provider) {
		std::shared_ptr<Aws::Auth::AWSCredentialsProvider> base = makeBaseProvider(config.providerType);
		if (config.roleArn.empty()) {
			provider = base;
		} else {
			std::shared_ptr<Aws::STS::STSClient> stsClient =
			    std::make_shared<Aws::STS::STSClient>(base, stsClientConfiguration(config));
			provider = std::make_shared<Aws::Auth::STSAssumeRoleCredentialsProvider>(
			    config.roleArn.c_str(),
			    Aws::String("fdbbackup"),
			    config.externalId.c_str(),
			    Aws::Auth::DEFAULT_CREDS_LOAD_FREQ_SECONDS,
			    stsClient);
		}
	}
	return provider;
}

} // namespace

namespace FDBAWSCredentialsProvider {

Aws::Auth::AWSCredentials getAwsCredentials(const AwsCredentialConfig& config) {
	initSdkOnce();
	return providerFor(config)->GetAWSCredentials();
}

} // namespace FDBAWSCredentialsProvider

#endif
