// SPDX-License-Identifier: MIT
#include "ServiceGrant.h"

#include <openssl/cms.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <map>
#include <memory>
#include <sstream>
#include <vector>

namespace med::service {
namespace {

template <typename T, void (*F)(T*)>
struct Free {
    void operator()(T* p) const { F(p); }
};
using Bio = std::unique_ptr<BIO, Free<BIO, BIO_free_all>>;
using Cms = std::unique_ptr<CMS_ContentInfo, Free<CMS_ContentInfo, CMS_ContentInfo_free>>;
using Store = std::unique_ptr<X509_STORE, Free<X509_STORE, X509_STORE_free>>;

Verdict refuse(const std::string& why) {
    Verdict v;
    v.reason = why;
    return v;
}

std::string opensslError() {
    unsigned long e = ERR_get_error();
    if (e == 0) return "unknown OpenSSL error";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    ERR_clear_error();
    return buf;
}

bool isBase64(const std::string& s) {
    if (s.empty() || s.size() % 4 != 0) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum || c == '+' || c == '/') continue;
        // '=' only as padding, at the end
        if (c == '=' && i + 2 >= s.size()) continue;
        return false;
    }
    return true;
}

bool decodeBase64(const std::string& in, std::vector<unsigned char>& out) {
    if (!isBase64(in)) return false;
    out.assign(in.size() / 4 * 3, 0);
    int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(in.data()),
                            static_cast<int>(in.size()));
    if (n < 0) return false;
    std::size_t pad = 0;
    if (in[in.size() - 1] == '=') ++pad;
    if (in[in.size() - 2] == '=') ++pad;
    out.resize(static_cast<std::size_t>(n) - pad);
    return true;
}

bool printable(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7e) return false;
    return true;
}

bool plainToken(const std::string& s, std::size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (char c : s) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '@' || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

const std::set<std::string> kKeyTypes = {
    "ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
};

}  // namespace

std::string keyFingerprint(const std::string& blobBase64) {
    std::vector<unsigned char> blob;
    if (!decodeBase64(blobBase64, blob)) return {};
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(blob.data(), blob.size(), md, &len, EVP_sha256(), nullptr) != 1) return {};
    std::vector<unsigned char> b64(4 * ((len + 2) / 3) + 1, 0);
    EVP_EncodeBlock(b64.data(), md, static_cast<int>(len));
    std::string s(reinterpret_cast<char*>(b64.data()));
    while (!s.empty() && s.back() == '=') s.pop_back();
    return "SHA256:" + s;
}

Verdict parseGrant(const std::string& text) {
    if (text.size() > kMaxGrantBytes) return refuse("grant larger than " + std::to_string(kMaxGrantBytes) + " bytes");
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != "med-service-grant 1")
        return refuse("not a version 1 service grant");

    std::map<std::string, std::string> fields;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (!printable(line)) return refuse("control character in the grant");
        std::size_t sp = line.find(' ');
        if (sp == std::string::npos || sp == 0) return refuse("malformed line in the grant");
        std::string key = line.substr(0, sp), value = line.substr(sp + 1);
        static const std::set<std::string> known = {"compatible", "duration", "nonce", "key", "reason"};
        if (!known.count(key)) return refuse("unknown field '" + key + "' in the grant");
        if (!fields.emplace(key, value).second) return refuse("field '" + key + "' given twice");
    }
    for (const char* required : {"compatible", "duration", "nonce", "key"})
        if (!fields.count(required)) return refuse(std::string("grant without '") + required + "'");

    Verdict v;
    Grant& g = v.grant;
    g.compatible = fields["compatible"];
    if (!plainToken(g.compatible, 128)) return refuse("malformed compatible");

    const std::string& d = fields["duration"];
    if (d.empty() || d.size() > 6 || d.find_first_not_of("0123456789") != std::string::npos)
        return refuse("malformed duration");
    g.durationS = static_cast<unsigned>(std::stoul(d));
    if (g.durationS < kMinDurationS || g.durationS > kMaxDurationS)
        return refuse("duration outside " + std::to_string(kMinDurationS) + ".." +
                      std::to_string(kMaxDurationS) + " s");

    g.nonce = fields["nonce"];
    if (g.nonce.size() < 16 || g.nonce.size() > 64 ||
        g.nonce.find_first_not_of("0123456789abcdef") != std::string::npos)
        return refuse("malformed nonce (16 to 64 lower-case hex digits)");

    // "type blob [comment]" and nothing else: a line that starts with
    // anything but a known type (command="...", from=, environment=) is an
    // options field, and options are the device's to choose, not the grant's.
    std::istringstream key(fields["key"]);
    key >> g.keyType >> g.keyBlob;
    std::getline(key, g.keyComment);
    if (!g.keyComment.empty() && g.keyComment[0] == ' ') g.keyComment.erase(0, 1);
    if (!kKeyTypes.count(g.keyType)) return refuse("key type not accepted: '" + g.keyType + "'");
    std::vector<unsigned char> blob;
    if (!decodeBase64(g.keyBlob, blob)) return refuse("key is not base64");
    // The blob names its own type (RFC 4253 6.6): a mismatch is a forged line.
    if (blob.size() < 4 + g.keyType.size()) return refuse("key blob too short");
    std::size_t tlen = (std::size_t(blob[0]) << 24) | (std::size_t(blob[1]) << 16) |
                       (std::size_t(blob[2]) << 8) | std::size_t(blob[3]);
    if (tlen != g.keyType.size() ||
        std::string(blob.begin() + 4, blob.begin() + 4 + static_cast<long>(tlen)) != g.keyType)
        return refuse("key blob does not match its type");
    if (!g.keyComment.empty() && !plainToken(g.keyComment, 64)) return refuse("malformed key comment");

    g.reason = fields.count("reason") ? fields["reason"] : "";
    if (g.reason.size() > 200) return refuse("reason longer than 200 characters");

    g.fingerprint = keyFingerprint(g.keyBlob);
    v.ok = true;
    return v;
}

Verdict verifyGrant(const std::string& der, const std::string& keyringPem) {
    if (der.empty() || der.size() > kMaxGrantBytes) return refuse("grant missing or too large");
    ERR_clear_error();

    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.data());
    Cms cms(d2i_CMS_ContentInfo(nullptr, &p, static_cast<long>(der.size())));
    if (!cms) return refuse("not a CMS structure: " + opensslError());
    if (OBJ_obj2nid(CMS_get0_type(cms.get())) != NID_pkcs7_signed) return refuse("CMS is not SignedData");

    Store store(X509_STORE_new());
    Bio kb(BIO_new_mem_buf(keyringPem.data(), static_cast<int>(keyringPem.size())));
    STACK_OF(X509_INFO)* infos = PEM_X509_INFO_read_bio(kb.get(), nullptr, nullptr, nullptr);
    if (!infos) return refuse("keyring unreadable: " + opensslError());
    int certs = 0, crls = 0;
    for (int i = 0; i < sk_X509_INFO_num(infos); ++i) {
        X509_INFO* info = sk_X509_INFO_value(infos, i);
        if (info->x509 && X509_STORE_add_cert(store.get(), info->x509) == 1) ++certs;
        if (info->crl && X509_STORE_add_crl(store.get(), info->crl) == 1) ++crls;
    }
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
    // The same demands RAUC's check-crl=true makes: no CRL, no verification.
    if (certs == 0) return refuse("keyring holds no certificate");
    if (crls == 0) return refuse("keyring holds no CRL");
    X509_STORE_set_flags(store.get(), X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
    // smime_sign, CMS_verify's default purpose, rejects a codeSigning-only
    // certificate (rauc system.conf, check-purpose). The purpose is checked
    // below instead, against the extended key usage itself.
    X509_STORE_set_purpose(store.get(), X509_PURPOSE_ANY);

    Bio out(BIO_new(BIO_s_mem()));
    if (CMS_verify(cms.get(), nullptr, store.get(), nullptr, out.get(), CMS_BINARY) != 1)
        return refuse("signature does not verify: " + opensslError());

    STACK_OF(X509)* signers = CMS_get0_signers(cms.get());
    int nsigners = signers ? sk_X509_num(signers) : 0;
    if (nsigners != 1) {
        sk_X509_free(signers);
        return refuse("grant must have exactly one signer, has " + std::to_string(nsigners));
    }
    X509* signer = sk_X509_value(signers, 0);
    bool codesign = (X509_get_extension_flags(signer) & EXFLAG_XKUSAGE) &&
                    (X509_get_extended_key_usage(signer) & XKU_CODE_SIGN);
    char name[256];
    X509_NAME_oneline(X509_get_subject_name(signer), name, sizeof name);
    sk_X509_free(signers);
    if (!codesign) return refuse("signer is not a code-signing certificate");

    char* data = nullptr;
    long len = BIO_get_mem_data(out.get(), &data);
    Verdict v = parseGrant(std::string(data, static_cast<std::size_t>(len)));
    v.grant.signer = name;
    return v;
}

Verdict checkGrant(const Verdict& verified, const std::string& compatible,
                   const std::set<std::string>& usedNonces) {
    if (!verified.ok) return verified;
    if (verified.grant.compatible != compatible)
        return refuse("grant is for '" + verified.grant.compatible + "', this device is '" + compatible + "'");
    if (usedNonces.count(verified.grant.nonce)) return refuse("grant already used (nonce " + verified.grant.nonce + ")");
    return verified;
}

std::string authorizedKeysLine(const Grant& grant) {
    std::string line = "restrict,pty " + grant.keyType + " " + grant.keyBlob;
    line += " grant-" + grant.nonce;
    return line + "\n";
}

}  // namespace med::service
