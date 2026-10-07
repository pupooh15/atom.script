//=========================================================================
//	AtsValue：スクリプトとやり取りする値（ats_value の C# 版）
//	文字列は string で持ち、ネイティブに渡すときだけ UTF-8 にする
//=========================================================================
using System;
using System.Runtime.InteropServices;
using AtomScript.Interop;

namespace AtomScript
{
	public readonly struct AtsValue
	{
		public readonly AtsType Type;
		readonly ulong	m_bits;		// bool / int / enum / handle
		readonly float	m_float;
		readonly string	m_string;

		AtsValue( AtsType type, ulong bits, float f, string s )
		{
			Type = type; m_bits = bits; m_float = f; m_string = s;
		}

		public static readonly AtsValue Void = default;

		public static AtsValue Bool( bool v )			=> new AtsValue( AtsType.Bool, v ? 1ul : 0ul, 0, null );
		public static AtsValue Int( int v )				=> new AtsValue( AtsType.Int, (ulong)(uint)v, 0, null );
		public static AtsValue Float( float v )			=> new AtsValue( AtsType.Float, 0, v, null );
		public static AtsValue String( string v )		=> new AtsValue( AtsType.String, 0, 0, v ?? string.Empty );
		public static AtsValue Enum( int v )			=> new AtsValue( AtsType.Enum, (ulong)(uint)v, 0, null );
		public static AtsValue Handle( AtsHandle v )	=> new AtsValue( AtsType.Handle, v.Value, 0, null );

		public static implicit operator AtsValue( bool v )		=> Bool( v );
		public static implicit operator AtsValue( int v )		=> Int( v );
		public static implicit operator AtsValue( float v )		=> Float( v );
		public static implicit operator AtsValue( string v )	=> String( v );
		public static implicit operator AtsValue( AtsHandle v )	=> Handle( v );

		public bool			AsBool		=> m_bits != 0;
		public int			AsInt		=> Type == AtsType.Float ? (int)m_float : (int)(uint)m_bits;
		public float		AsFloat		=> Type == AtsType.Float ? m_float : (int)(uint)m_bits;
		public string		AsString	=> m_string ?? string.Empty;
		public AtsHandle	AsHandle	=> new AtsHandle( m_bits );

		public override string ToString()
		{
			switch( Type ){
			case AtsType.Bool:		return AsBool ? "true" : "false";
			case AtsType.Int:		return AsInt.ToString();
			case AtsType.Enum:		return $"enum({AsInt})";
			case AtsType.Float:		return m_float.ToString( System.Globalization.CultureInfo.InvariantCulture );
			case AtsType.String:	return "\"" + AsString + "\"";
			case AtsType.Handle:	return AsHandle.ToString();
			default:				return "void";
			}
		}

		//---------------------------------------------------------------------
		// ネイティブとの変換
		//---------------------------------------------------------------------
		// 文字列なら CoTaskMem に UTF-8 を確保して str に返す（呼び出し側が FreeCoTaskMem する）
		internal NativeValue ToNative( out IntPtr str )
		{
			NativeValue n = default;
			n.type = (uint)Type;
			str = IntPtr.Zero;
			switch( Type ){
			case AtsType.Bool:		n.i = m_bits != 0 ? 1 : 0; break;
			case AtsType.Int:
			case AtsType.Enum:		n.i = (int)(uint)m_bits; break;
			case AtsType.Float:		n.f = m_float; break;
			case AtsType.Handle:	n.h = m_bits; break;
			case AtsType.String:
				str = Marshal.StringToCoTaskMemUTF8( AsString );
				n.s = str;
				break;
			}
			return n;
		}

		internal static AtsValue FromNative( in NativeValue n )
		{
			switch( (AtsType)n.type ){
			case AtsType.Bool:		return Bool( n.i != 0 );
			case AtsType.Int:		return Int( n.i );
			case AtsType.Enum:		return Enum( n.i );
			case AtsType.Float:		return Float( n.f );
			case AtsType.String:	return String( Utf8.ToManaged( n.s ) );
			case AtsType.Handle:	return Handle( new AtsHandle( n.h ) );
			default:				return Void;
			}
		}
	}

	// 引数の配列をネイティブに渡すための一時領域（using で文字列を解放する）
	internal struct NativeArgs : IDisposable
	{
		public NativeValue[]	Values;
		IntPtr[]				m_strings;

		public NativeArgs( AtsValue[] args )
		{
			Values = null;
			m_strings = null;
			if( args == null || args.Length == 0 ) return;
			Values = new NativeValue[ args.Length ];
			for( int i = 0; i < args.Length; ++i ){
				Values[i] = args[i].ToNative( out IntPtr s );
				if( s != IntPtr.Zero ){
					if( m_strings == null ) m_strings = new IntPtr[ args.Length ];
					m_strings[i] = s;
				}
			}
		}

		public int Count => Values?.Length ?? 0;

		public void Dispose()
		{
			if( m_strings == null ) return;
			foreach( IntPtr s in m_strings ) if( s != IntPtr.Zero ) Marshal.FreeCoTaskMem( s );
			m_strings = null;
		}
	}
}
