/**
 * @file TestOidcJwt.cpp
 * @brief RS256 ID Token 校验与 PKCE 的固定向量
 * @author Gyanis
 * @date 2026-10-21
 */

#include "Core/Auth/Jwt.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
    constexpr const char *kModulus =
            "wBovdIS79SSnVk9NY2DSJ4Ld1c_Qyq0b-T52IGCCaRKc7Q0hVumFBw88_mq7zkda-_KyTfTd1sC5-MO9Z9BCdJXmLlAnJOmO3XB09YUkS-Y85L3258BQH4dzKBDbhm9jCBEgHZGTduWoxHThJTyYrBdbqorh7DsCnK-"
            "bucwY16jG_ZurJL60qWBynmQcA8VMEnadyPpW7yRZ5jtaXxVi__fiqf-oCUG7YE1zNS1y2plb6-5LvOSBmkvpMxJMi2YB7etTPbuMidHu2vcj_aG1yN_DiOwYhYEgyJw1Xv3UqXapWCkL8qR_QbME14GzqMdCrGrJQ-"
            "T0CbF51TSkcsufIQ";
    constexpr const char *kExponent = "AQAB";
    constexpr const char *kToken    = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6InRlc3Qta2V5LTEifQ.eyJpc3MiOiJodHRwczovL2lkcC5leGFtcGxlIiwiYXVkIjoiZWRnZXB1bHNlIiwic3ViIjoidXNlci0xMjMiLCJlbWFpbCI6Im9pZGMudXNlckBleGFtcGxlLmNvbSIsInByZWZlcnJlZF91c2VybmFtZSI6Im9pZGN1c2VyIiwibmFtZSI6Ik9JREMgVXNlciIsIm5vbmNlIjoibm9uY2UtMTIzIiwiZXhwIjo0MTAyNDQ0ODAwLCJpYXQiOjE3NjAwMDAwMDB9.Kt2WLy62K5dK9adxpbDrJkAPSPFrL8YYK6-cpEnTc3XWlRWqnry-NcT0G0P_dlPTyEolfi4pKg9MDYUsC_xNDSu9DXrqthyfZ8orsBO53bP1FKtQ6cks5aggVXjlHCPH2EqLjldZ5pXMLP4FAdew1tOVG8CB69htcp2O9JFoBSeEWLUzZMEPGJ1b5Mg5VcdNsLdPrtonF3ibKgfHXDXGGYR4Nskz2hZAZJygMDZrhYNCr2FUbhCBwgBolO502TB8t87FmEwf0vZ-Fa62E_Bih0SBiGTf-r1UekOg75NexoPrdjakGQDUqpfdZ_BwmxTMEGB3SSrvB6iaXgFCqV7mAQ";

    [[nodiscard]] std::vector<EdgePulse::Auth::JwtRsaKey> testKeys()
    {
        return {EdgePulse::Auth::JwtRsaKey{"test-key-1", kModulus, kExponent}};
    }
} // namespace

// 钉住：RS256 签名、iss/aud/exp/nonce 校验与声明读取
TEST(OidcJwt, VerifiesRs256TokenAndClaims)
{
    const auto valid = EdgePulse::Auth::verifyRs256Jwt(kToken, testKeys(), "https://idp.example", "edgepulse", "nonce-123", 1760000100);
    ASSERT_TRUE(valid.ok) << valid.error;
    EXPECT_EQ("user-123", valid.claims.subject);
    EXPECT_EQ("oidc.user@example.com", valid.claims.email);
    EXPECT_EQ("oidcuser", valid.claims.preferredUsername);

    EXPECT_FALSE(EdgePulse::Auth::verifyRs256Jwt(kToken, testKeys(), "https://idp.example", "edgepulse", "wrong-nonce", 1760000100).ok);
    EXPECT_FALSE(EdgePulse::Auth::verifyRs256Jwt(kToken, testKeys(), "https://idp.example", "other-client", "nonce-123", 1760000100).ok);
    EXPECT_FALSE(EdgePulse::Auth::verifyRs256Jwt(kToken, testKeys(), "https://idp.example", "edgepulse", "nonce-123", 4102444801).ok);
}

// 钉住：篡改签名后校验失败
TEST(OidcJwt, RejectsTamperedSignature)
{
    std::string tampered{kToken};
    tampered[tampered.size() - 2] = tampered[tampered.size() - 2] == 'A' ? 'B' : 'A';
    EXPECT_FALSE(EdgePulse::Auth::verifyRs256Jwt(tampered, testKeys(), "https://idp.example", "edgepulse", "nonce-123", 1760000100).ok);
}

// 钉住：PKCE S256 与 RFC 7636 附录 B 的向量一致
TEST(OidcJwt, PkceMatchesRfc7636Vector)
{
    EXPECT_EQ("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
              EdgePulse::Auth::pkceS256Challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"));
    const std::string expectedBytes{static_cast<char>(1), static_cast<char>(0), static_cast<char>(1)};
    EXPECT_EQ(expectedBytes, EdgePulse::Auth::decodeBase64Url(kExponent).value_or(""));
}
