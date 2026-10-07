//=========================================================================
//	AtsScriptAsset：.ats をインポートしたアセット（コンパイル済みの .atsb を持つ）
//	エディタの AtsImporter が作る。ゲームからは ScriptRuntime.LoadProgram( asset ) で読み込む。
//=========================================================================
using System;
using UnityEngine;

namespace AtomScript
{
	public sealed class AtsScriptAsset : ScriptableObject
	{
		[SerializeField] byte[]	m_bytecode = Array.Empty<byte>();
		[SerializeField] string	m_scriptId = string.Empty;
		[SerializeField] string	m_manifestHash = string.Empty;		// 16 桁の 16 進（ulong はシリアライズの都合で文字列にする）

		// .atsb のバイト列（変更しないこと）
		public byte[]	Bytecode		=> m_bytecode;
		public string	ScriptId		=> m_scriptId;
		// 生成元のマニフェストのハッシュ（atsc gen が出力する Registration.ManifestHash と比べられる）
		public ulong	ManifestHash	=> ulong.TryParse( m_manifestHash, System.Globalization.NumberStyles.HexNumber, null, out ulong h ) ? h : 0;

		// インポーター用
		public static AtsScriptAsset Create( byte[] bytecode, string scriptId, ulong manifestHash )
		{
			var a = CreateInstance<AtsScriptAsset>();
			a.m_bytecode = bytecode ?? Array.Empty<byte>();
			a.m_scriptId = scriptId ?? string.Empty;
			a.m_manifestHash = manifestHash.ToString( "x16" );
			return a;
		}
	}
}
