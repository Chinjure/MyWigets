// probe_nested_job.cpp - 验证：进程已在一个 Job 中时，能否再追加到另一个 Job
// （sandbox 版挂起器会 AssignProcessToJobObject；基准程序要用自己的 Job 记账）。
#include <windows.h>
#include <cstdio>
#include <string>

static ULONGLONG JobCpuMs(HANDLE job) {
    JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION info{};
    if (!QueryInformationJobObject(job, JobObjectBasicAndIoAccountingInformation,
                                   &info, sizeof(info), nullptr)) {
        return 0;
    }
    ULARGE_INTEGER u{}, k{};
    u.QuadPart = info.BasicInfo.TotalUserTime.QuadPart;
    k.QuadPart = info.BasicInfo.TotalKernelTime.QuadPart;
    return (u.QuadPart + k.QuadPart) / 10000ull;
}

int wmain() {
    HANDLE jobA = CreateJobObjectW(nullptr, nullptr);
    HANDLE jobB = CreateJobObjectW(nullptr, nullptr);
    wchar_t cmd[] = L"C:\\Windows\\System32\\cmd.exe";
    wchar_t args[] = L"/c ping -n 20 127.0.0.1 >nul";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(cmd, args, nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, nullptr, &si, &pi)) {
        std::printf("CreateProcess failed %lu\n", GetLastError());
        return 1;
    }
    BOOL a = AssignProcessToJobObject(jobA, pi.hProcess);
    std::printf("assign to A: %d err=%lu\n", a, a ? 0 : GetLastError());
    BOOL b = AssignProcessToJobObject(jobB, pi.hProcess);
    std::printf("assign to B (nested): %d err=%lu\n", b, b ? 0 : GetLastError());
    ResumeThread(pi.hThread);
    Sleep(3000);
    std::printf("jobA cpu=%llu ms jobB cpu=%llu ms\n", JobCpuMs(jobA), JobCpuMs(jobB));
    TerminateJobObject(jobA, 1);
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(jobA);
    CloseHandle(jobB);
    return 0;
}
