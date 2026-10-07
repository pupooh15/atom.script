//=========================================================================
//	ScriptProgram：.atsb 1 つ分（読み取り専用。複数の VM で共有できる）
//=========================================================================
using System;
using AtomScript.Interop;

namespace AtomScript
{
	public sealed class ScriptProgram : IDisposable
	{
		readonly ProgramSafeHandle m_handle = new ProgramSafeHandle();

		public ScriptRuntime	Runtime		{ get; }
		public string			ScriptId	{ get; }

		internal IntPtr Ptr => m_handle.IsClosed ? throw new ObjectDisposedException( nameof( ScriptProgram ) ) : m_handle.DangerousGetHandle();

		internal ScriptProgram( ScriptRuntime rt, IntPtr p )
		{
			Runtime = rt;
			m_handle.Set( p, rt.Handle );
			ScriptId = Utf8.ToManaged( NativeMethods.ats_program_script_id( p ) );
		}

		// VM に結び付いている間は、Dispose してもネイティブ側の実体は残る
		public void Dispose() => m_handle.Dispose();

		public override string ToString() => $"ScriptProgram({ScriptId})";
	}
}
