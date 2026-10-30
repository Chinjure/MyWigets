using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

internal static class WakeSample
{
    [DllImport("kernel32.dll")]
    private static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")]
    private static extern bool QueryThreadCycleTime(IntPtr h, out ulong cycles);
    [DllImport("kernel32.dll")]
    private static extern ulong GetTickCount64();
    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    public static int Main(string[] args)
    {
        int seconds = int.Parse(args[0], CultureInfo.InvariantCulture);
        int intervalMs = int.Parse(args[1], CultureInfo.InvariantCulture);
        string outPath = args[2];
        uint[] tids = new uint[args.Length - 3];
        for (int i = 3; i < args.Length; i++)
        {
            tids[i - 3] = uint.Parse(args[i], CultureInfo.InvariantCulture);
        }

        var handles = new IntPtr[tids.Length];
        var prev = new ulong[tids.Length];
        for (int i = 0; i < tids.Length; i++)
        {
            handles[i] = OpenThread(0x0040, false, tids[i]);
            ulong c = 0;
            if (handles[i] != IntPtr.Zero) QueryThreadCycleTime(handles[i], out c);
            prev[i] = c;
        }

        var sb = new StringBuilder();
        sb.AppendLine("t_ms,tid,cycles");
        ulong t0 = GetTickCount64();
        while ((long)(GetTickCount64() - t0) < seconds * 1000L)
        {
            ulong now = GetTickCount64();
            long t = (long)(now - t0);
            for (int i = 0; i < tids.Length; i++)
            {
                if (handles[i] == IntPtr.Zero) continue;
                ulong c = 0;
                if (!QueryThreadCycleTime(handles[i], out c)) continue;
                ulong d = c - prev[i];
                prev[i] = c;
                if (d > 0)
                {
                    sb.Append(t).Append(',').Append(tids[i]).Append(',').Append(d).AppendLine();
                }
            }
            Thread.Sleep(intervalMs);
        }
        for (int i = 0; i < handles.Length; i++)
        {
            if (handles[i] != IntPtr.Zero) CloseHandle(handles[i]);
        }
        File.WriteAllText(outPath, sb.ToString(), new UTF8Encoding(false));
        Console.WriteLine("wrote " + outPath);
        return 0;
    }
}
