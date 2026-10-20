// 0x4795b0 - event queue writer (proven; not a projectile spawner)
// binary: /gundx.exe-902b77
// ghidra: http://127.0.0.1:8765/mcp


/* event_post: queues one event. Type 1 (size 0x1b): u8 type, u32 event_id, u16 param, i16 a, i16 b,
   f32 x/y/z, u32 extra. Type 3 (size 0x17): same without i16 a/b. Buffers: 0x62fbc8/0x62fbc0 (modes
   2/3), else 0x79cf80/0x79cf6c; overflow check at 0x750e. Type 1/3 events are GENERATE events, not
   a projectile spawner. */

void event_post(int param_1,undefined2 param_2,int param_3,int param_4)

{
  undefined4 uVar1;
  undefined1 *puVar2;
  int iVar3;
  undefined4 in_stack_0000002c;
  
  iVar3 = param_1;
  if ((DAT_0083d620 != 1) && (param_1 != 0)) {
    if ((DAT_0083d620 == 2) || (DAT_0083d620 == 3)) {
      if (0x750e < DAT_0062fbc0) {
        FUN_00479730(param_1,&param_3,&param_1);
        uVar1 = FUN_00463a50(iVar3,param_3,param_1);
        FUN_0040c530(s_Maximum_event_buffer2_size_reach_004bb02c,uVar1);
        return;
      }
      puVar2 = &DAT_0062fbc8 + DAT_0062fbc0;
    }
    else {
      if (0x750e < DAT_0079cf6c) {
        FUN_004797a0(param_1,&param_3,&param_1);
        uVar1 = FUN_00463a50(iVar3,param_3,param_1);
        FUN_0040c530(s_Maximum_event_buffer_size_reache_004bb080,uVar1);
        return;
      }
      puVar2 = &DAT_0079cf80 + DAT_0079cf6c;
    }
    if ((param_3 == 0) && (param_4 == 0)) {
      *puVar2 = 3;
      *(int *)(puVar2 + 1) = param_1;
      *(undefined2 *)(puVar2 + 5) = param_2;
      uVar1 = __ftol();
      *(undefined4 *)(puVar2 + 7) = uVar1;
      uVar1 = __ftol();
      *(undefined4 *)(puVar2 + 0xb) = uVar1;
      uVar1 = __ftol();
      *(undefined4 *)(puVar2 + 0xf) = uVar1;
      *(undefined4 *)(puVar2 + 0x13) = in_stack_0000002c;
      iVar3 = 0x17;
    }
    else {
      *puVar2 = 1;
      *(int *)(puVar2 + 1) = param_1;
      *(undefined2 *)(puVar2 + 5) = param_2;
      *(short *)(puVar2 + 7) = (short)param_3;
      *(short *)(puVar2 + 9) = (short)param_4;
      uVar1 = __ftol();
      *(undefined4 *)(puVar2 + 0xb) = uVar1;
      uVar1 = __ftol();
      *(undefined4 *)(puVar2 + 0xf) = uVar1;
      uVar1 = __ftol();
      *(undefined4 *)(puVar2 + 0x13) = uVar1;
      *(undefined4 *)(puVar2 + 0x17) = in_stack_0000002c;
      iVar3 = 0x1b;
    }
    if ((DAT_0083d620 != 2) && (DAT_0083d620 != 3)) {
      DAT_0079cf6c = DAT_0079cf6c + iVar3;
      return;
    }
    DAT_0062fbc0 = DAT_0062fbc0 + iVar3;
  }
  return;
}


