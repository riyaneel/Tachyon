using TachyonIpc.Native;

namespace TachyonIpc;

public sealed class TachyonException : Exception
{
    public TachyonError Error { get; }

    internal TachyonException(TachyonError error)
        : base($"Tachyon native error: {error} ({(int)error})")
    {
        Error = error;
    }

    internal TachyonException(TachyonError error, string context)
        : base($"Tachyon native error in {context}: {error} ({(int)error})")
    {
        Error = error;
    }

    internal static void ThrowIfError(TachyonError error, string context)
    {
        if (error != TachyonError.Success)
            throw new TachyonException(error, context);
    }
}