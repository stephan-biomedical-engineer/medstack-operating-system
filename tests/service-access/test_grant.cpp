// Host tests for the service grant verifier (med-service-access, ACE-03).
//
// The PKI and the grants are made by gen.sh with the openssl command line -
// an implementation independent of the verifier's libcrypto calls - so a
// grant that verifies here was signed the way a manufacturer would sign it,
// not the way the verifier expects.

#include "ServiceGrant.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace med::service;

static int failures = 0, checks = 0;
static std::string B;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        ++checks;                                                             \
        if (!(cond)) {                                                        \
            ++failures;                                                       \
            std::printf("  FALHA %s (%s:%d)\n", what, __FILE__, __LINE__);    \
        }                                                                     \
    } while (0)

static std::string slurp(const std::string& name) {
    std::ifstream f(B + "/" + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

static Verdict verify(const std::string& p7s, const std::string& keyring = "keyring.pem") {
    return verifyGrant(slurp(p7s), slurp(keyring));
}

static void refused(const Verdict& v, const char* expect, const char* what) {
    ++checks;
    if (v.ok || v.reason.find(expect) == std::string::npos) {
        ++failures;
        std::printf("  FALHA %s: esperado recusa com '%s', veio ok=%d '%s'\n", what, expect, v.ok, v.reason.c_str());
    }
}

static std::string header() { return "med-service-grant 1\n"; }

static std::string body(const std::string& key) {
    return "compatible med-os-test\nduration 1800\nnonce 0123456789abcdef0123456789abcdef\nkey " + key + "\n";
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "uso: %s DIR (de gen.sh)\n", argv[0]);
        return 2;
    }
    B = argv[1];
    const std::string blob = trim(slurp("tech.blob"));
    const std::string key = "ssh-ed25519 " + blob + " tech@bench";

    // --- the grant a manufacturer signs
    Verdict good = verify("good.p7s");
    CHECK(good.ok, "a concessão bem assinada verifica");
    if (!good.ok) std::printf("    motivo: %s\n", good.reason.c_str());
    CHECK(good.grant.compatible == "med-os-test", "compatible lido");
    CHECK(good.grant.durationS == 1800, "duração lida");
    CHECK(good.grant.reason == "bench test", "motivo lido");
    CHECK(good.grant.keyComment == "tech@bench", "comentário da chave lido");
    CHECK(good.grant.signer.find("CN=good") != std::string::npos, "assinante identificado");
    CHECK(good.grant.fingerprint == trim(slurp("tech.fingerprint")), "impressão digital igual à do ssh-keygen -l");
    CHECK(checkGrant(good, "med-os-test", {}).ok, "aceita neste dispositivo");
    CHECK(authorizedKeysLine(good.grant) ==
              "restrict,pty ssh-ed25519 " + blob + " grant-0123456789abcdef0123456789abcdef\n",
          "linha de authorized_keys: restrict,pty, chave, e nada vindo do texto além dela");

    // --- the device's own checks
    refused(checkGrant(good, "med-os-other", {}), "this device is", "outro compatible");
    refused(checkGrant(good, "med-os-test", {"0123456789abcdef0123456789abcdef"}), "already used",
            "nonce já usado (uma janela por concessão)");

    // --- signature and trust
    refused(verify("tampered.p7s"), "does not verify", "um byte do conteúdo alterado");
    refused(verify("foreign.p7s"), "does not verify", "assinada por uma CA que o dispositivo não tem");
    refused(verify("revoked.p7s"), "does not verify", "certificado revogado na CRL");
    refused(verify("noeku.p7s"), "not a code-signing", "certificado sem extendedKeyUsage");
    refused(verify("email.p7s"), "not a code-signing", "certificado de e-mail");
    refused(verify("good.p7s", "keyring-nocrl.pem"), "no CRL", "chaveiro sem CRL");
    refused(verify("plain.p7s"), "not a CMS", "texto sem assinatura");
    refused(verifyGrant("", slurp("keyring.pem")), "missing", "concessão vazia");
    refused(verifyGrant(std::string(kMaxGrantBytes + 1, 'x'), slurp("keyring.pem")), "too large",
            "concessão grande demais");

    // --- the text: strict, and nothing in it reaches authorized_keys as options
    CHECK(parseGrant(header() + body(key)).ok, "o texto de referência passa");
    refused(parseGrant("med-service-grant 2\n" + body(key)), "version 1", "versão desconhecida");
    refused(parseGrant(header() + body("command=\"/bin/sh\" " + key)), "key type not accepted",
            "opções antes da chave (command=)");
    refused(parseGrant(header() + body("ssh-rsa " + blob)), "key type not accepted", "tipo fora da lista");
    refused(parseGrant(header() + body("ecdsa-sha2-nistp256 " + blob)), "does not match its type",
            "blob de ed25519 declarado como ecdsa");
    refused(parseGrant(header() + body("ssh-ed25519 not*base64")), "not base64", "blob que não é base64");
    refused(parseGrant(header() + body(key + " $(reboot)")), "malformed key comment", "comentário com shell");
    refused(parseGrant(header() + body(key) + "duration 60\n"), "given twice", "campo repetido");
    refused(parseGrant(header() + body(key) + "permit root\n"), "unknown field", "campo desconhecido");
    refused(parseGrant(header() + body(key) + "reason a\tb\n"), "control character", "caractere de controle");
    refused(parseGrant(header() + "compatible med-os-test\nduration 1800\nkey " + key + "\n"), "without 'nonce'",
            "sem nonce");
    std::string b = body(key);
    auto with = [&](const std::string& from, const std::string& to) {
        std::string t = b;
        t.replace(t.find(from), from.size(), to);
        return header() + t;
    };
    refused(parseGrant(with("duration 1800", "duration 30")), "duration outside", "duração abaixo do mínimo");
    refused(parseGrant(with("duration 1800", "duration 28801")), "duration outside", "duração acima do máximo");
    refused(parseGrant(with("duration 1800", "duration -5")), "malformed duration", "duração negativa");
    refused(parseGrant(with("nonce 0123456789abcdef0123456789abcdef", "nonce 0123456789ABCDEF")), "malformed nonce",
            "nonce maiúsculo");
    refused(parseGrant(with("nonce 0123456789abcdef0123456789abcdef", "nonce 01234567")), "malformed nonce",
            "nonce curto");
    refused(parseGrant(with("compatible med-os-test", "compatible med os")), "malformed compatible",
            "compatible com espaço");
    CHECK(parseGrant(with("duration 1800", "duration 60")).ok, "duração mínima aceita");
    CHECK(parseGrant(with("duration 1800", "duration 28800")).ok, "duração máxima aceita");

    std::printf("service-access: %d verificações, %d falhas\n", checks, failures);
    return failures ? 1 : 0;
}
