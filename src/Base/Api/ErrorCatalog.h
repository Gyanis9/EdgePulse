/**
 * @file ErrorCatalog.h
 * @brief 统一业务码定义
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

namespace EdgePulse::Common
{
    inline constexpr int kCodeOk                  = 200;
    inline constexpr int kCodeBadRequest          = 400;
    inline constexpr int kCodeUnauthorized        = 401;
    inline constexpr int kCodeForbidden           = 403;
    inline constexpr int kCodeNotFound            = 404;
    inline constexpr int kCodeConflict            = 409;
    inline constexpr int kCodeTooManyRequests     = 429;
    inline constexpr int kCodeInternalServerError = 500;
    inline constexpr int kCodeServiceUnavailable  = 503;
} // namespace EdgePulse::Common
