// 0x477f90 - weapon target/aim scan (hypothesis)
// binary: /gundx.exe-902b77
// ghidra: http://127.0.0.1:8765/mcp


int FUN_00477f90(int param_1,undefined4 *param_2,int param_3,double param_4,double param_5,
                undefined4 *param_6,uint param_7)

{
  uint uVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  undefined4 *puVar5;
  double *pdVar6;
  uint *puVar7;
  undefined4 *puVar8;
  float10 fVar9;
  int local_11c;
  double *local_118;
  uint *local_114;
  double local_110;
  undefined8 local_108;
  double local_100;
  double local_f4;
  double local_ec;
  double local_e4;
  int local_dc;
  undefined1 local_d8 [4];
  undefined1 local_d4 [4];
  double local_d0;
  double local_c8;
  double local_c0;
  undefined1 local_b8 [8];
  double local_b0;
  double local_a8;
  double local_a0;
  undefined4 local_98 [6];
  undefined4 local_80 [6];
  undefined4 local_68 [25];
  
  iVar4 = param_1 * 0x3510;
  iVar2 = FUN_004277cd((&DAT_0081c1b8)[param_1 * 0xd44]);
  if (iVar2 != 0) {
    if (1.0 < *(double *)(&DAT_0081c020 + iVar4)) {
      param_4 = param_4 / *(double *)(&DAT_0081c020 + iVar4);
      param_5 = param_5 / *(double *)(&DAT_0081c020 + iVar4);
    }
    uVar1 = (&DAT_0081c240)[param_1 * 0xd44];
    pdVar6 = &local_b0;
    for (iVar3 = 6; iVar3 != 0; iVar3 = iVar3 + -1) {
      *(undefined4 *)pdVar6 = *param_2;
      param_2 = param_2 + 1;
      pdVar6 = (double *)((int)pdVar6 + 4);
    }
    if (((uVar1 & 0x80000) == 0) && (*(double *)(&DAT_0081bea0 + iVar4) != 0.0)) {
      FUN_0041e025(local_98,local_80,local_68,(&DAT_00819060)[param_1 * 0xd44],
                   (&DAT_00819064)[param_1 * 0xd44],
                   *(double *)(&DAT_00819058 + param_1 * 0xd44) + *(double *)(&DAT_0081bea0 + iVar4)
                   ,(&DAT_00819068)[param_1 * 0xd44],(&DAT_0081906c)[param_1 * 0xd44]);
    }
    else {
      puVar5 = (undefined4 *)(iVar2 + 0x18);
      puVar8 = local_98;
      for (iVar3 = 6; iVar3 != 0; iVar3 = iVar3 + -1) {
        *puVar8 = *puVar5;
        puVar5 = puVar5 + 1;
        puVar8 = puVar8 + 1;
      }
      puVar5 = (undefined4 *)(iVar2 + 0x30);
      puVar8 = local_80;
      for (iVar3 = 6; iVar3 != 0; iVar3 = iVar3 + -1) {
        *puVar8 = *puVar5;
        puVar5 = puVar5 + 1;
        puVar8 = puVar8 + 1;
      }
      puVar5 = (undefined4 *)(iVar2 + 0x48);
      puVar8 = local_68;
      for (iVar3 = 6; iVar3 != 0; iVar3 = iVar3 + -1) {
        *puVar8 = *puVar5;
        puVar5 = puVar5 + 1;
        puVar8 = puVar8 + 1;
      }
    }
    local_108 = (double)param_3;
    local_11c = 0;
    local_114 = &DAT_0081c240;
    iVar2 = local_dc;
    local_110 = local_108;
    do {
      puVar7 = local_114;
      if ((((local_11c != param_1) && (local_114[4] != 0)) && (local_114[4] != 7)) &&
         (((local_114[-0x22] != 0 && ((*local_114 & 0x40000) == 0)) &&
          (((param_7 & *local_114) == 0 &&
           ((((byte)DAT_0083d184 & 1) == 0 || (local_114[0x93] != (&DAT_0081c48c)[param_1 * 0xd44]))
           )))))) {
        local_118 = (double *)FUN_004277cd(local_114[-0x22]);
        if (local_118 != (double *)0x0) {
          local_f4 = *local_118 - local_b0;
          local_ec = local_118[1] - local_a8;
          local_e4 = local_118[2] - local_a0;
          fVar9 = (float10)FUN_004344df(local_f4,local_ec,local_e4);
          local_100 = (double)fVar9;
          if (fVar9 < (float10)local_110) {
            FUN_00437909();
            if (((0.0 < local_c8) &&
                (ABS(local_d0) < *(double *)(puVar7 + -0x86) * local_c8 * param_4)) &&
               (ABS(local_c0) < *(double *)(puVar7 + -0x84) * local_c8 * param_5)) {
              FUN_004226e9(&local_f4);
              iVar3 = FUN_0043c51f((&DAT_0081c1b8)[param_1 * 0xd44],(&DAT_0081bdb0)[param_1 * 0xd44]
                                   ,(&DAT_0081bdb4)[param_1 * 0xd44],
                                   (&DAT_0081bdb8)[param_1 * 0xd44],(&DAT_0081bdbc)[param_1 * 0xd44]
                                   ,(&DAT_0081bdc0)[param_1 * 0xd44],
                                   *(undefined4 *)(&DAT_0081bdc4 + iVar4),&local_b0,&local_f4,
                                   local_b8,&local_118,&local_dc,local_d8,local_d4,0,0x40200000,
                                   (undefined4)local_108,local_108._4_4_,0,0,0,0,0);
              if ((iVar3 == 0) && ((double *)puVar7[-0x22] == local_118)) {
                pdVar6 = &local_f4;
                puVar5 = param_6;
                for (iVar3 = 6; puVar7 = local_114, iVar2 = local_11c, local_110 = local_100,
                    iVar3 != 0; iVar3 = iVar3 + -1) {
                  *puVar5 = *(undefined4 *)pdVar6;
                  pdVar6 = (double *)((int)pdVar6 + 4);
                  puVar5 = puVar5 + 1;
                }
              }
            }
          }
        }
      }
      local_114 = puVar7 + 0xd44;
      local_11c = local_11c + 1;
    } while ((int)local_114 < 0x836ac0);
    local_11c = 0;
    local_114 = &DAT_007ac3bc;
    do {
      puVar7 = local_114;
      if (((((byte)local_114[9] & 5) == 1) &&
          ((local_114[9] & 1 << ((char)param_1 + 9U & 0x1f)) == 0)) && (*local_114 != 0)) {
        local_118 = (double *)FUN_004277cd(*local_114);
        if (local_118 != (double *)0x0) {
          local_f4 = *local_118 - local_b0;
          local_ec = local_118[1] - local_a8;
          local_e4 = local_118[2] - local_a0;
          fVar9 = (float10)FUN_004344df(local_f4,local_ec,local_e4);
          local_100 = (double)fVar9;
          if (fVar9 < (float10)local_110) {
            FUN_00437909();
            if (((0.0 < local_c8) && (ABS(local_d0) < local_c8 * param_4)) &&
               (ABS(local_c0) < local_c8 * param_5)) {
              FUN_004226e9(&local_f4);
              iVar3 = FUN_0043c51f((&DAT_0081c1b8)[param_1 * 0xd44],(&DAT_0081bdb0)[param_1 * 0xd44]
                                   ,(&DAT_0081bdb4)[param_1 * 0xd44],
                                   (&DAT_0081bdb8)[param_1 * 0xd44],(&DAT_0081bdbc)[param_1 * 0xd44]
                                   ,(&DAT_0081bdc0)[param_1 * 0xd44],
                                   *(undefined4 *)(&DAT_0081bdc4 + iVar4),&local_b0,&local_f4,
                                   local_b8,&local_118,&local_dc,local_d8,local_d4,0,0x40100000,
                                   (undefined4)local_108,local_108._4_4_,0,0,0,0,0);
              if ((iVar3 == 0) && ((double *)*puVar7 == local_118)) {
                pdVar6 = &local_f4;
                puVar5 = param_6;
                for (iVar2 = 6; iVar2 != 0; iVar2 = iVar2 + -1) {
                  *puVar5 = *(undefined4 *)pdVar6;
                  pdVar6 = (double *)((int)pdVar6 + 4);
                  puVar5 = puVar5 + 1;
                }
                iVar2 = local_11c + 8;
                puVar7 = local_114;
                local_110 = local_100;
              }
            }
          }
        }
      }
      local_114 = puVar7 + 0x14a;
      local_11c = local_11c + 1;
    } while ((int)local_114 < 0x80005c);
    if (local_110 < local_108) {
      return iVar2;
    }
  }
  return -1;
}


