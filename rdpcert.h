#ifndef RDPCERT_H
#define RDPCERT_H

#include <string>

namespace rdpcert {

struct Certificate {
	std::string host;
	int port;
	std::string commonName;
	std::string subject;
	std::string issuer;
	std::string fingerprint;
	int flags;
};

enum class CertResult {
	Reject = 0,
	AcceptPermanently = 1,
	AcceptTemporarily = 2,
};

} // namespace rdpcert

#endif // RDPCERT_H
