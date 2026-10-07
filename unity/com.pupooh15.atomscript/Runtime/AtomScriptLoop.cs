//=========================================================================
//	AtomScriptLoop：PlayerLoop の Update の先頭に専用の段を差し込み、登録された VM を更新する
//	（MonoBehaviour の Update の実行順に左右されないようにする）
//=========================================================================
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.LowLevel;

namespace AtomScript
{
	public static class AtomScriptLoop
	{
		// PlayerLoop に差し込む段の型（目印）
		public struct UpdateVMs {}

		static readonly List<ScriptVM>	s_vms = new List<ScriptVM>();
		static ScriptVM[]				s_work = new ScriptVM[ 4 ];

		public static void Register( ScriptVM vm )
		{
			if( vm == null || vm.IsDisposed || s_vms.Contains( vm ) ) return;
			Install();
			s_vms.Add( vm );
		}

		public static void Unregister( ScriptVM vm ) => s_vms.Remove( vm );

		public static int Count => s_vms.Count;

		[RuntimeInitializeOnLoadMethod( RuntimeInitializeLoadType.SubsystemRegistration )]
		static void ResetStatics() => s_vms.Clear();		// ドメインリロードを切っている場合に備える

		static void Install()
		{
			PlayerLoopSystem root = PlayerLoop.GetCurrentPlayerLoop();
			if( root.subSystemList == null ) return;
			for( int i = 0; i < root.subSystemList.Length; ++i ){
				ref PlayerLoopSystem phase = ref root.subSystemList[i];
				if( phase.type != typeof( UnityEngine.PlayerLoop.Update ) ) continue;
				PlayerLoopSystem[] subs = phase.subSystemList ?? new PlayerLoopSystem[ 0 ];
				foreach( PlayerLoopSystem s in subs ) if( s.type == typeof( UpdateVMs ) ) return;		// 差し込み済み
				var list = new PlayerLoopSystem[ subs.Length + 1 ];
				list[0] = new PlayerLoopSystem { type = typeof( UpdateVMs ), updateDelegate = Tick };
				subs.CopyTo( list, 1 );
				phase.subSystemList = list;
				PlayerLoop.SetPlayerLoop( root );
				return;
			}
		}

		// 更新中に VM が破棄・登録されてもよいように、複製してから回す
		static void Tick()
		{
			int n = s_vms.Count;
			if( n == 0 ) return;
			if( s_work.Length < n ) s_work = new ScriptVM[ n * 2 ];
			s_vms.CopyTo( s_work );
			float dt = Time.deltaTime;
			for( int i = 0; i < n; ++i ){
				ScriptVM vm = s_work[i];
				s_work[i] = null;
				if( !vm.IsDisposed ) vm.Update( dt );
			}
		}
	}
}
