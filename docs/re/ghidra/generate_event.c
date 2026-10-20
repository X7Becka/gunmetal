// 0x479960 - GENERATE wrapper: door toggle + entity sub-script (proven)
// binary: /gundx.exe-902b77
// ghidra: http://127.0.0.1:8765/mcp


/* generate_event / GENERATE wrapper: calls door_toggle_by_id(id) unless mode==1, then
   generate_runner(id,...) to run the entity sub-script. Does not spawn an object itself. */

void generate_event(undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
                   undefined4 param_5,undefined4 param_6,undefined4 param_7,undefined4 param_8)

{
  if (DAT_0083d620 != 1) {
    FUN_0044f690(param_1);
  }
  generate_runner(param_1,param_2,param_3,param_4,param_5,param_6,param_7,param_8);
  return;
}


