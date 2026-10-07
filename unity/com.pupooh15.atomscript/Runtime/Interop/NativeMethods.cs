//=========================================================================
//	ats_api.h の P/Invoke 宣言
//	構造体のレイアウトは ats_api.h と一致させる。文字列は UTF-8 の IntPtr で渡す（Utf8 を使う）
//=========================================================================
using System;
using System.Runtime.InteropServices;

namespace AtomScript.Interop
{
	// ats_value（16 バイト。union は 8 バイト目から）
	[StructLayout( LayoutKind.Explicit, Size = 16 )]
	internal struct NativeValue
	{
		[FieldOffset( 0 )] public uint		type;
		[FieldOffset( 4 )] public uint		reserved;
		[FieldOffset( 8 )] public int		i;		// INT / ENUM / BOOL
		[FieldOffset( 8 )] public float		f;
		[FieldOffset( 8 )] public IntPtr	s;
		[FieldOffset( 8 )] public ulong		h;
	}

	[StructLayout( LayoutKind.Sequential )]
	internal struct RuntimeDesc
	{
		public uint		size;
		public IntPtr	alloc;
		public IntPtr	free;
		public IntPtr	log;
		public IntPtr	user;
	}

	[StructLayout( LayoutKind.Sequential )]
	internal struct CommandDesc
	{
		public uint		size;
		public IntPtr	name;
		public uint		sigHash;
		public IntPtr	channel;
		public IntPtr	fn;
		public IntPtr	cancel;
		public IntPtr	user;
	}

	[StructLayout( LayoutKind.Sequential )]
	internal struct QueryDesc
	{
		public uint		size;
		public IntPtr	name;
		public uint		sigHash;
		public IntPtr	fn;
		public IntPtr	user;
	}

	[StructLayout( LayoutKind.Sequential )]
	internal struct VarDesc
	{
		public uint			size;
		public uint			id;
		public IntPtr		name;
		public uint			type;
		public uint			scope;
		public NativeValue	init;
	}

	[StructLayout( LayoutKind.Sequential )]
	internal struct VmDesc
	{
		public uint		size;
		public uint		instructionBudget;
		public uint		maxStack;
		public uint		maxCallDepth;
		public ulong	randomSeed;
	}

	// コールバック（IL2CPP では [MonoPInvokeCallback] 付きの static メソッドだけを渡す）
	[UnmanagedFunctionPointer( CallingConvention.Cdecl )] internal delegate void		LogFn( int level, IntPtr message, IntPtr user );
	[UnmanagedFunctionPointer( CallingConvention.Cdecl )] internal delegate int		CommandFn( IntPtr call, IntPtr user );
	[UnmanagedFunctionPointer( CallingConvention.Cdecl )] internal delegate void		CancelFn( IntPtr vm, ulong token, IntPtr user );
	[UnmanagedFunctionPointer( CallingConvention.Cdecl )] internal delegate int		QueryFn( IntPtr call, IntPtr user );
	[UnmanagedFunctionPointer( CallingConvention.Cdecl )] internal delegate int		WriteFn( IntPtr data, UIntPtr size, IntPtr user );
	[UnmanagedFunctionPointer( CallingConvention.Cdecl )] internal delegate int		ReadFn( IntPtr data, UIntPtr size, IntPtr user );

	internal static class NativeMethods
	{
#if (UNITY_IOS || UNITY_SWITCH || UNITY_PS5 || UNITY_GAMECORE) && !UNITY_EDITOR
		const string Lib = "__Internal";		// 静的リンク
#else
		const string Lib = "atomscript";
#endif
		const CallingConvention CC = CallingConvention.Cdecl;

		public const int ApiVersionMajor = 1;

		[DllImport( Lib, CallingConvention = CC )] public static extern uint	ats_get_api_version();
		[DllImport( Lib, CallingConvention = CC )] public static extern IntPtr	ats_result_string( int r );

		// ランタイム
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_runtime_create( ref RuntimeDesc desc, out IntPtr rt );
		[DllImport( Lib, CallingConvention = CC )] public static extern void	ats_runtime_destroy( IntPtr rt );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_register_command( IntPtr rt, ref CommandDesc desc );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_register_query( IntPtr rt, ref QueryDesc desc );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_define_var( IntPtr rt, ref VarDesc desc );
		[DllImport( Lib, CallingConvention = CC )] public static extern IntPtr	ats_runtime_last_error( IntPtr rt );

		// プログラム
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_program_load( IntPtr rt, byte[] bytes, UIntPtr size, out IntPtr prog );
		[DllImport( Lib, CallingConvention = CC )] public static extern void	ats_program_release( IntPtr prog );
		[DllImport( Lib, CallingConvention = CC )] public static extern IntPtr	ats_program_script_id( IntPtr prog );

		// VM
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_create( IntPtr rt, ref VmDesc desc, out IntPtr vm );
		[DllImport( Lib, CallingConvention = CC )] public static extern void	ats_vm_destroy( IntPtr vm );
		[DllImport( Lib, CallingConvention = CC )] public static extern IntPtr	ats_vm_last_error( IntPtr vm );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_attach( IntPtr vm, IntPtr prog );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_detach( IntPtr vm, IntPtr prog );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_update( IntPtr vm, float dt );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_fire_event( IntPtr vm, IntPtr prog, IntPtr ev, NativeValue[] args, int argc, out ulong fiber );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_broadcast_event( IntPtr vm, IntPtr ev, NativeValue[] args, int argc, out int count );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_abort_fiber( IntPtr vm, ulong fiber );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_is_fiber_alive( IntPtr vm, ulong fiber );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_fiber_count( IntPtr vm );

		// ハンドラから使う関数
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_call_argc( IntPtr call );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_arg_int( IntPtr call, int index );
		[DllImport( Lib, CallingConvention = CC )] public static extern float	ats_arg_float( IntPtr call, int index );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_arg_bool( IntPtr call, int index );
		[DllImport( Lib, CallingConvention = CC )] public static extern IntPtr	ats_arg_string( IntPtr call, int index );
		[DllImport( Lib, CallingConvention = CC )] public static extern ulong	ats_arg_handle( IntPtr call, int index );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_call_set_result( IntPtr call, ref NativeValue value );
		[DllImport( Lib, CallingConvention = CC )] public static extern ulong	ats_call_get_token( IntPtr call );
		[DllImport( Lib, CallingConvention = CC )] public static extern ulong	ats_call_fiber( IntPtr call );
		[DllImport( Lib, CallingConvention = CC )] public static extern IntPtr	ats_call_vm( IntPtr call );

		// 待機中の呼び出しの完了（result は NULL 可なので 2 通り用意する）
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_call_complete( IntPtr vm, ulong token, ref NativeValue result );
		[DllImport( Lib, CallingConvention = CC, EntryPoint = "ats_call_complete" )]
		public static extern int ats_call_complete_void( IntPtr vm, ulong token, IntPtr nullResult );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_call_fail( IntPtr vm, ulong token, IntPtr reason );

		// 変数
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_var_get( IntPtr vm, uint id, out NativeValue value );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_var_set( IntPtr vm, uint id, ref NativeValue value );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_script_reset( IntPtr vm, IntPtr scriptId );

		// セーブ
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_save( IntPtr vm, IntPtr write, IntPtr user );
		[DllImport( Lib, CallingConvention = CC )] public static extern int		ats_vm_load( IntPtr vm, IntPtr read, IntPtr user );
	}
}
