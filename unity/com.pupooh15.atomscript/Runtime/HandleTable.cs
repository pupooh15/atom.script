//=========================================================================
//	HandleTable：オブジェクトと AtsHandle の対応表（使うかどうかはゲーム側の自由）
//	破棄済みの UnityEngine.Object は見つからない扱いにする。
//=========================================================================
using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace AtomScript
{
	public sealed class HandleTable<T> where T : class
	{
		sealed class RefComparer : IEqualityComparer<T>
		{
			public bool Equals( T a, T b ) => ReferenceEquals( a, b );
			public int GetHashCode( T o ) => RuntimeHelpers.GetHashCode( o );
		}

		readonly Dictionary<ulong, T>	m_byHandle = new Dictionary<ulong, T>();
		readonly Dictionary<T, ulong>	m_byObject = new Dictionary<T, ulong>( new RefComparer() );
		ulong							m_next = 1;

		public int Count => m_byHandle.Count;

		// 登録済みなら同じハンドルを返す
		public AtsHandle Add( T obj )
		{
			if( obj == null ) return AtsHandle.None;
			if( m_byObject.TryGetValue( obj, out ulong h ) ) return new AtsHandle( h );
			h = m_next++;
			m_byHandle.Add( h, obj );
			m_byObject.Add( obj, h );
			return new AtsHandle( h );
		}

		public bool TryGet( AtsHandle handle, out T obj )
		{
			if( m_byHandle.TryGetValue( handle.Value, out obj ) && !IsDestroyed( obj ) ) return true;
			obj = null;
			return false;
		}

		public T Get( AtsHandle handle ) => TryGet( handle, out T obj ) ? obj : null;

		public bool Remove( AtsHandle handle )
		{
			if( !m_byHandle.TryGetValue( handle.Value, out T obj ) ) return false;
			m_byHandle.Remove( handle.Value );
			m_byObject.Remove( obj );
			return true;
		}

		public bool Remove( T obj ) => obj != null && m_byObject.TryGetValue( obj, out ulong h ) && Remove( new AtsHandle( h ) );

		public void Clear()
		{
			m_byHandle.Clear();
			m_byObject.Clear();
		}

		static bool IsDestroyed( T obj ) => obj is UnityEngine.Object u && u == null;
	}
}
