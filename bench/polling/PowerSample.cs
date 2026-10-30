using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

internal static class PowerSample
{
    [StructLayout(LayoutKind.Sequential)]
    private struct PDH_FMT_COUNTERVALUE
    {
        public uint CStatus;
        public double doubleValue;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PDH_FMT_COUNTERVALUE_ITEM_W
    {
        public IntPtr szName;
        public PDH_FMT_COUNTERVALUE FmtValue;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESSENTRY
    {
        public uint dwSize;
        public uint cntUsage;
        public uint th32ProcessID;
        public IntPtr th32DefaultHeapID;
        public uint th32ModuleID;
        public uint cntThreads;
        public uint th32ParentProcessID;
        public int pcPriClassBase;
        public uint dwFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FILETIME
    {
        public uint dwLowDateTime;
        public uint dwHighDateTime;
        public ulong ToU64() { return ((ulong)dwHighDateTime << 32) | dwLowDateTime; }
    }

    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhOpenQueryW(string src, IntPtr userData, out IntPtr query);

    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhAddEnglishCounterW(IntPtr query, string path, IntPtr userData, out IntPtr counter);

    [DllImport("pdh.dll")]
    private static extern uint PdhCollectQueryData(IntPtr query);

    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhGetFormattedCounterArrayW(IntPtr counter, uint fmt, ref uint bufSize, ref uint itemCount, IntPtr buffer);

    [DllImport("pdh.dll")]
    private static extern uint PdhCloseQuery(IntPtr query);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);

    [DllImport("kernel32.dll")]
    private static extern bool GetProcessTimes(IntPtr proc, out FILETIME creation, out FILETIME exit, out FILETIME kernel, out FILETIME user);

    [DllImport("kernel32.dll")]
    private static extern bool GetSystemTimes(out FILETIME idle, out FILETIME kernel, out FILETIME user);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr h);

    [DllImport("kernel32.dll")]
    private static extern ulong GetTickCount64();

    private const uint PDH_FMT_DOUBLE = 0x00000200;
    private const uint PDH_MORE_DATA = 0x800007D2;

    private static double[] ReadInstances(IntPtr counter, string want1, string want2)
    {
        uint size = 0, count = 0;
        uint st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, ref size, ref count, IntPtr.Zero);
        if (st != PDH_MORE_DATA)
        {
            return null;
        }
        IntPtr buf = Marshal.AllocHGlobal((int)size);
        try
        {
            st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, ref size, ref count, buf);
            if (st != 0)
            {
                return null;
            }
            double v1 = double.NaN, v2 = double.NaN;
            int itemSize = Marshal.SizeOf(typeof(PDH_FMT_COUNTERVALUE_ITEM_W));
            for (int i = 0; i < count; i++)
            {
                IntPtr p = new IntPtr(buf.ToInt64() + (long)i * itemSize);
                PDH_FMT_COUNTERVALUE_ITEM_W item = (PDH_FMT_COUNTERVALUE_ITEM_W)Marshal.PtrToStructure(p, typeof(PDH_FMT_COUNTERVALUE_ITEM_W));
                string name = Marshal.PtrToStringUni(item.szName) ?? string.Empty;
                if (string.Equals(name, want1, StringComparison.OrdinalIgnoreCase)) v1 = item.FmtValue.doubleValue;
                else if (string.Equals(name, want2, StringComparison.OrdinalIgnoreCase)) v2 = item.FmtValue.doubleValue;
            }
            return new[] { v1, v2 };
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
    }

    public static int Main(string[] args)
    {
        if (args.Length < 4)
        {
            Console.WriteLine("usage: PowerSample <seconds> <interval_ms> <pid> <out.csv>");
            return 2;
        }
        double seconds = double.Parse(args[0], CultureInfo.InvariantCulture);
        int intervalMs = int.Parse(args[1], CultureInfo.InvariantCulture);
        uint pid = uint.Parse(args[2], CultureInfo.InvariantCulture);
        string outPath = args[3];

        IntPtr query, counter;
        if (PdhOpenQueryW(null, IntPtr.Zero, out query) != 0) { Console.WriteLine("pdh open failed"); return 3; }
        if (PdhAddEnglishCounterW(query, "\\Energy Meter(*)\\Power", IntPtr.Zero, out counter) != 0) { Console.WriteLine("pdh counter failed"); return 3; }
        PdhCollectQueryData(query);
        Thread.Sleep(intervalMs);
        PdhCollectQueryData(query);

        IntPtr proc = OpenProcess(0x0400 /*QUERY_INFORMATION*/, false, pid);
        if (proc == IntPtr.Zero) { Console.WriteLine("open process failed: " + Marshal.GetLastWin32Error()); return 4; }

        var sb = new StringBuilder();
        sb.AppendLine("t_ms,pkg_mw,pp0_mw,proc_cpu_ms,sys_busy_pct");
        ulong t0 = GetTickCount64();
        FILETIME c1, e1, k1, u1, si1, sk1, su1;
        GetProcessTimes(proc, out c1, out e1, out k1, out u1);
        GetSystemTimes(out si1, out sk1, out su1);
        ulong prevBusy = sk1.ToU64() + su1.ToU64() - si1.ToU64();
        ulong prevT = t0;

        long target = (long)(seconds * 1000.0);
        while (true)
        {
            ulong now = GetTickCount64();
            long t = (long)(now - t0);
            if (t >= target) break;

            PdhCollectQueryData(query);
            double[] v = ReadInstances(counter, "rapl_package0_pkg", "rapl_package0_pp0");
            FILETIME c, e, k, u, si, sk, su;
            GetProcessTimes(proc, out c, out e, out k, out u);
            GetSystemTimes(out si, out sk, out su);
            double procCpuMs = (k.ToU64() + u.ToU64()) / 10000.0;
            ulong busy = sk.ToU64() + su.ToU64() - si.ToU64();
            double dtMs = (now > prevT) ? (now - prevT) : 0.0;
            double busyPct = dtMs > 0 ? (busy - prevBusy) / 10000.0 / dtMs * 100.0 : 0.0;
            prevBusy = busy;
            prevT = now;
            sb.Append(t).Append(',');
            sb.Append(double.IsNaN(v[0]) ? "" : v[0].ToString("F1", CultureInfo.InvariantCulture)).Append(',');
            sb.Append(double.IsNaN(v[1]) ? "" : v[1].ToString("F1", CultureInfo.InvariantCulture)).Append(',');
            sb.Append(procCpuMs.ToString("F1", CultureInfo.InvariantCulture)).Append(',');
            sb.Append(busyPct.ToString("F2", CultureInfo.InvariantCulture));
            sb.AppendLine();

            long spent = (long)(GetTickCount64() - now);
            int sleep = intervalMs - (int)spent;
            if (sleep > 0) Thread.Sleep(sleep);
        }

        File.WriteAllText(outPath, sb.ToString(), new UTF8Encoding(false));
        CloseHandle(proc);
        PdhCloseQuery(query);
        Console.WriteLine("wrote " + outPath);
        return 0;
    }
}

