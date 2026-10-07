//=========================================================================
//	ネイティブのオブジェクトを包む SafeHandle
//	VM とプログラムは親のランタイムに参照を持ち、ランタイムより先に解放されるようにする
//	（ファイナライザで解放される場合も順序が保たれる）
//=========================================================================
using System;
using System.Runtime.InteropServices;

namespace AtomScript.Interop
{
	internal sealed class RuntimeSafeHandle : SafeHandle
	{
		public RuntimeSafeHandle() : base( IntPtr.Zero, true ) {}
		public override bool IsInvalid => handle == IntPtr.Zero;

		internal void Set( IntPtr p ) => SetHandle( p );

		protected override bool ReleaseHandle()
		{
			NativeMethods.ats_runtime_destroy( handle );
			return true;
		}
	}

	// ランタイムに依存するオブジェクト（VM・プログラム）の共通部分
	internal abstract class ChildSafeHandle : SafeHandle
	{
		RuntimeSafeHandle m_parent;

		protected ChildSafeHandle() : base( IntPtr.Zero, true ) {}
		public override bool IsInvalid => handle == IntPtr.Zero;

		// 作成に成功したあとで呼ぶ
		internal void Set( IntPtr p, RuntimeSafeHandle parent )
		{
			bool added = false;
			parent.DangerousAddRef( ref added );
			m_parent = parent;
			SetHandle( p );
		}

		protected abstract void Destroy( IntPtr p );

		protected override bool ReleaseHandle()
		{
			Destroy( handle );
			m_parent?.DangerousRelease();
			m_parent = null;
			return true;
		}
	}

	internal sealed class VmSafeHandle : ChildSafeHandle
	{
		protected override void Destroy( IntPtr p ) => NativeMethods.ats_vm_destroy( p );
	}

	internal sealed class ProgramSafeHandle : ChildSafeHandle
	{
		protected override void Destroy( IntPtr p ) => NativeMethods.ats_program_release( p );
	}
}
