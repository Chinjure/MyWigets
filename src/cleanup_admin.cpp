// cleanup_admin.cpp - one-shot elevated cleanup helper.
// Run explicitly as administrator; no resident service, no polling.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include "app_cleanup.h"

namespace {
bool NoTray(const appcleanup::CleanupRequest&, const std::vector<DWORD>&) {
    return false;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring app;
    std::wstring pin;
    bool dryRun = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--app") == 0 && i + 1 < argc) {
            app = argv[++i];
        } else if (_wcsicmp(argv[i], L"--pin") == 0 && i + 1 < argc) {
            pin = argv[++i];
        } else if (_wcsicmp(argv[i], L"--dry-run") == 0) {
            dryRun = true;
        } else if (_wcsicmp(argv[i], L"--help") == 0) {
            wprintf(L"MyWigetsCleanup.exe --app <exe> [--pin <pin>] [--dry-run]\n");
            return 0;
        }
    }
    if (app.empty()) {
        wprintf(L"missing --app\n");
        return 2;
    }

    appcleanup::CleanupRequest request;
    request.appKey = appsuspend::NormalizePath(app);
    request.pinKey = appsuspend::NormalizePath(pin);
    request.displayName = appsuspend::BaseNameOf(request.appKey);
    request.allAppKeys.push_back(request.appKey);

    appcleanup::CleanupOptions options;
    options.dryRun = dryRun;
    options.stopServices = true;
    options.minProcessAgeMs = 0;

    const appcleanup::CleanupResult result =
        appcleanup::CleanupOnce(request, options, NoTray);
    wprintf(L"verdict=%d matched=%zu terminated=%zu denied=%zu servicesStopped=%zu servicesDenied=%zu\n%s\n",
            static_cast<int>(result.verdict), result.matched, result.terminated,
            result.denied, result.servicesStopped, result.servicesDenied,
            result.detail.c_str());
    const bool ok = !result.needsElevation &&
        (result.verdict == appcleanup::CleanupVerdict::Cleaned ||
         result.verdict == appcleanup::CleanupVerdict::NoProcesses ||
         result.verdict == appcleanup::CleanupVerdict::DryRun);
    return ok ? 0 : 1;
}