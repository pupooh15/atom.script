#==========================================================================
#	コアのソースを Unity の IL2CPP 用にコピーする
#	cmake -DINPUT=<ファイル> -DOUTPUT=<ファイル> -P copy_il2cpp_source.cmake
#	IL2CPP はプラグインのソースを 1 つのフォルダーに平らにコピーしてコンパイルするので、
#	#include "atomscript/xxx.h" を #include "xxx.h" に書き換える。中身が同じなら書き換えない。改行は LF。
#==========================================================================
cmake_minimum_required( VERSION 3.16 )		# -P で動かすときもポリシーを決める

file( READ "${INPUT}" text )
string( REPLACE "#include \"atomscript/" "#include \"" text "${text}" )
string( REGEX REPLACE "\n$" "" text "${text}" )		# configure_file が最後の行に改行を足すので、ここで 1 つ落とす
file( WRITE "${OUTPUT}.in" "@text@" )
configure_file( "${OUTPUT}.in" "${OUTPUT}.tmp" @ONLY NEWLINE_STYLE UNIX )
execute_process( COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUTPUT}.tmp" "${OUTPUT}" )
file( REMOVE "${OUTPUT}.in" "${OUTPUT}.tmp" )
