// 0x45fa00 - GENERATE id -> entity sub-script via VM (proven)
// binary: /gundx.exe-902b77
// ghidra: http://127.0.0.1:8765/mcp


/* generate_runner: looks up the generate/entity id in 0x5e4680 and runs its sub-script through
   0x457390; logs 'event %ld (%s)' and undefined events. Not an object factory. */

void generate_runner(int param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
                    undefined4 param_5,undefined4 param_6,undefined4 param_7,uint param_8)

{
  uint uVar1;
  undefined4 uVar2;
  int iVar3;
  undefined4 *puVar4;
  int local_8;
  int local_4;
  
  local_4 = 0;
  if (DAT_005a46d4 != 0) {
    uVar2 = FUN_00463a50(param_1);
    FUN_0040c530(s_event__ld___s__004b8a8c,param_1,uVar2);
  }
  local_8 = 0;
  if (0 < DAT_005b1df0) {
    puVar4 = &DAT_00616658;
    do {
      if ((&DAT_005e4680)[local_8] == param_1) {
        local_4 = local_4 + 1;
        if ((param_8 & 0xffff) < 8) {
          uVar1 = (&DAT_005b0df0)[local_8];
          if ((uVar1 & 1 << ((byte)(param_8 & 0xffff) & 0x1f)) != 0) {
            if ((uVar1 & 0x200) != 0) {
              (&DAT_005b0df0)[local_8] = uVar1 & 0xfffffe00;
            }
            uVar2 = *puVar4;
LAB_0045fb01:
            FUN_00457390(uVar2,0,param_2,param_3,param_4,param_5,param_6,param_7,param_8);
          }
        }
        else {
          uVar1 = (&DAT_005b0df0)[local_8];
          if ((uVar1 & 0x100) != 0) {
            if ((uVar1 & 0x200) != 0) {
              (&DAT_005b0df0)[local_8] = uVar1 & 0xfffffe00;
            }
            uVar2 = *puVar4;
            goto LAB_0045fb01;
          }
        }
      }
      local_8 = local_8 + 1;
      puVar4 = puVar4 + 1;
    } while (local_8 < DAT_005b1df0);
  }
  if (((local_4 == 0) && (iVar3 = FUN_0045f9d0(param_1), -1 < iVar3)) &&
     (((&DAT_004b5128)[iVar3] & 1) == 0)) {
    FUN_0040c530(s_Warning____s__event_not_defined_004b8a68,
                 s_SYSTEM_LEVEL_STARTUP_004acde8 + iVar3 * 0x30);
    (&DAT_004b5128)[iVar3] = (&DAT_004b5128)[iVar3] | 1;
  }
  if (DAT_005a46d4 != 0) {
    FUN_0040c530(s_end_event__ld_004b8a58,param_1);
  }
  return;
}


