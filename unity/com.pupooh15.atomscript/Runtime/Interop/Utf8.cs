//=========================================================================
//	UTF-8 文字列の受け渡し
//=========================================================================
using System;
using System.Runtime.InteropServices;

namespace AtomScript.Interop
{
	// ネイティブに渡す UTF-8 文字列（using で解放する。null は NULL として渡す）
	internal struct Utf8 : IDisposable
	{
		public IntPtr Ptr { get; private set; }

		public Utf8( string s ) { Ptr = s == null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8( s ); }

		public void Dispose()
		{
			if( Ptr != IntPtr.Zero ) Marshal.FreeCoTaskMem( Ptr );
			Ptr = IntPtr.Zero;
		}

		// ネイティブの const char* を string にする（NULL は空文字列）
		public static string ToManaged( IntPtr p ) => p == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8( p );
	}
}
