// 0x473a00 - thing/AI target scan (hypothesis)
// binary: /gundx.exe-902b77
// ghidra: http://127.0.0.1:8765/mcp


/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

undefined4
FUN_00473a00(int param_1,double *param_2,undefined4 *param_3,undefined4 *param_4,undefined4 *param_5
            ,undefined4 *param_6,undefined4 *param_7)

{
  int iVar1;
  double *pdVar2;
  int iVar3;
  int *piVar4;
  int iVar5;
  undefined4 *puVar6;
  float10 fVar7;
  int local_78;
  int local_74;
  int *local_70;
  uint *local_6c;
  int local_68;
  int local_64;
  double local_60;
  undefined4 local_50 [4];
  undefined4 local_40;
  undefined4 uStack_3c;
  double local_38;
  double local_30;
  double local_28;
  double local_20;
  double local_18;
  
  *param_4 = 1;
  *param_7 = 0;
  param_7[1] = 0;
  *param_3 = 0;
  *param_6 = 0;
  param_6[1] = 0;
  local_60 = 1e+38;
  local_74 = -1;
  *param_5 = 0;
  param_5[1] = 0;
  local_78 = -1;
  iVar5 = param_1 * 0x528;
  if ((*(int *)(&DAT_007ac460 + iVar5) < 0) || (0x10b < *(int *)(&DAT_007ac460 + iVar5))) {
    *(undefined4 *)(&DAT_007ac460 + iVar5) = 0xffffffff;
  }
  if (((&DAT_007ac3e8)[param_1 * 0x14a] & 0x800) != 0) {
    local_64 = *(int *)(&DAT_007ac460 + iVar5);
    *(undefined4 *)(&DAT_007ac460 + iVar5) = 0xffffffff;
  }
  if ((((&DAT_007ac3e8)[param_1 * 0x14a] & 0x400) == 0) ||
     ((*(int *)(&DAT_007ac460 + iVar5) < 8 && (-1 < *(int *)(&DAT_007ac460 + iVar5))))) {
    local_6c = (uint *)0x0;
    local_68 = 0;
    local_70 = &DAT_0081c244;
    do {
      if ((((*(int *)(&DAT_007ac460 + iVar5) == -1) &&
           (((&DAT_007ac3e8)[param_1 * 0x14a] & 0x400) == 0)) ||
          ((uint *)*(int *)(&DAT_007ac460 + iVar5) == local_6c)) &&
         (((((&DAT_007ac3e0)[param_1 * 0x14a] & 1 << ((char)local_6c + 9U & 0x1f)) == 0 &&
           (local_70[3] != 0)) && ((local_70[3] != 7 && (local_70[-0x23] != 0)))))) {
        iVar3 = *(int *)(&DAT_007ac3b8 + iVar5);
        if (iVar3 < 1) {
          piVar4 = local_70 + -0xc81;
        }
        else {
          iVar1 = *local_70;
          if (iVar1 < iVar3) {
            iVar1 = (iVar1 - iVar3) + 0xf0;
          }
          else {
            iVar1 = iVar1 - iVar3;
          }
          piVar4 = (int *)(&DAT_00819070 + (iVar1 + local_68) * 0x18);
        }
        pdVar2 = &local_38;
        for (iVar3 = 6; iVar3 != 0; iVar3 = iVar3 + -1) {
          *(int *)pdVar2 = *piVar4;
          piVar4 = piVar4 + 1;
          pdVar2 = (double *)((int)pdVar2 + 4);
        }
        fVar7 = (float10)FUN_004344df(local_38 - *param_2,local_30 - param_2[1],
                                      local_28 - param_2[2]);
        if ((fVar7 < (float10)*(double *)(&DAT_007ac130 + iVar5)) && ((double)fVar7 < local_60)) {
          FUN_00437909();
          puVar6 = local_50;
          pdVar2 = &local_20;
          for (iVar3 = 6; iVar3 != 0; iVar3 = iVar3 + -1) {
            *(undefined4 *)pdVar2 = *puVar6;
            puVar6 = puVar6 + 1;
            pdVar2 = (double *)((int)pdVar2 + 4);
          }
          local_78 = (int)local_6c;
          local_60 = (double)fVar7;
        }
      }
      local_70 = local_70 + 0xd44;
      local_6c = (uint *)((int)local_6c + 1);
      local_68 = local_68 + 0x236;
    } while ((int)local_70 < 0x836ac4);
  }
  if (-1 < local_78) {
    local_74 = local_78;
  }
  if ((((&DAT_007ac3e8)[param_1 * 0x14a] & 0x100) != 0) || (7 < *(int *)(&DAT_007ac460 + iVar5))) {
    local_68 = 8;
    local_6c = &DAT_007ac3e0;
    local_60 = local_60 * _DAT_007ab9f8 + _DAT_007ac078;
    do {
      if ((((((((byte)*local_6c & 7) == 1) && ((local_6c[1] & 0x20000) == 0)) && (local_6c[-9] != 0)
            ) && ((((&DAT_007ac3e0)[param_1 * 0x14a] ^ *local_6c) & 0xfffffe00) != 0)) &&
          (((*(int *)(&DAT_007ac460 + iVar5) == -1 &&
            (((&DAT_007ac3e8)[param_1 * 0x14a] & 0x100) != 0)) ||
           (*(int *)(&DAT_007ac460 + iVar5) == local_68)))) &&
         (pdVar2 = (double *)FUN_004277cd(), pdVar2 != (double *)0x0)) {
        fVar7 = (float10)FUN_004344df(*pdVar2 - *param_2,pdVar2[1] - param_2[1],
                                      pdVar2[2] - param_2[2]);
        if ((fVar7 < (float10)*(double *)(&DAT_007ac130 + iVar5)) && ((double)fVar7 < local_60)) {
          FUN_00437909();
          local_78 = local_68;
          puVar6 = local_50;
          pdVar2 = &local_20;
          for (iVar3 = 6; local_60 = (double)fVar7, iVar3 != 0; iVar3 = iVar3 + -1) {
            *(undefined4 *)pdVar2 = *puVar6;
            puVar6 = puVar6 + 1;
            pdVar2 = (double *)((int)pdVar2 + 4);
          }
        }
      }
      local_6c = local_6c + 0x14a;
      iVar3 = local_68 + -7;
      local_68 = local_68 + 1;
    } while (iVar3 < 0x104);
  }
  if (local_78 < 0) goto LAB_00474025;
  pdVar2 = &local_20;
  puVar6 = local_50;
  for (iVar3 = 6; iVar3 != 0; iVar3 = iVar3 + -1) {
    *puVar6 = *(undefined4 *)pdVar2;
    pdVar2 = (double *)((int)pdVar2 + 4);
    puVar6 = puVar6 + 1;
  }
  if ((ABS(local_20) * 4.0 < local_18) && (*(int *)(&DAT_007ac464 + iVar5) == 0)) {
    if (*(int *)(&DAT_007ac468 + iVar5) == 0) {
      iVar1 = *(int *)(&DAT_007ac474 + iVar5);
      iVar3 = iVar1 + -1;
      if (iVar3 < 0) goto LAB_00473f1b;
      piVar4 = (int *)(&DAT_007ac498 + (iVar3 + param_1 * 0x14a) * 4);
      do {
        if (((piVar4[-8] != 0) && ((double)*piVar4 <= local_60)) && (local_60 <= (double)piVar4[8]))
        break;
        iVar3 = iVar3 + -1;
        piVar4 = piVar4 + -1;
      } while (-1 < iVar3);
    }
    else {
      iVar1 = *(int *)(&DAT_007ac474 + iVar5);
      iVar3 = 0;
      if (0 < iVar1) {
        piVar4 = (int *)(&DAT_007ac498 + iVar5);
        do {
          if (((piVar4[-8] != 0) && ((double)*piVar4 <= local_60)) &&
             (local_60 <= (double)piVar4[8])) break;
          iVar3 = iVar3 + 1;
          piVar4 = piVar4 + 1;
        } while (iVar3 < iVar1);
      }
    }
    if ((-1 < iVar3) && (iVar3 < iVar1)) {
      iVar3 = (iVar3 + param_1 * 0x14a) * 4;
      event_post(*(undefined4 *)(&DAT_007ac478 + iVar3),(&DAT_007ac448)[param_1 * 0x14a],0,0,
                 *(undefined4 *)param_2,*(undefined4 *)((int)param_2 + 4),
                 *(undefined4 *)(param_2 + 1),*(undefined4 *)((int)param_2 + 0xc),
                 *(undefined4 *)(param_2 + 2),*(undefined4 *)((int)param_2 + 0x14),
                 local_78 * 0x10000 + 8 + param_1);
      *(int *)(&DAT_007ac464 + iVar5) =
           *(int *)(&DAT_007ac464 + iVar5) + *(int *)(&DAT_007ac4d8 + iVar3);
      *(uint *)(&DAT_007ac468 + iVar5) = (uint)(*(int *)(&DAT_007ac468 + iVar5) == 0);
      if ((*(int *)(&DAT_007ac46c + iVar5) != 0) &&
         (iVar3 = FUN_00480e30(), iVar3 % *(int *)(&DAT_007ac46c + iVar5) == 0)) {
        *(int *)(&DAT_007ac464 + iVar5) =
             *(int *)(&DAT_007ac464 + iVar5) + *(int *)(&DAT_007ac470 + iVar5);
      }
    }
  }
LAB_00473f1b:
  *param_7 = local_40;
  param_7[1] = uStack_3c;
  if (0.0 <= (double)CONCAT44(local_50[3],local_50[2])) {
    fVar7 = (float10)FUN_00436774(local_50[0],local_50[1],local_50[2],local_50[3]);
    if ((0.0 < (double)CONCAT44(uStack_3c,local_40)) && (*(double *)(&DAT_007ac158 + iVar5) != 0.0))
    {
      fVar7 = fVar7 - (float10)(double)CONCAT44(uStack_3c,local_40);
    }
    if (((fVar7 <= (float10)*(int *)(&DAT_007ac3b4 + iVar5)) &&
        (*param_4 = 0, (*(uint *)(&DAT_007ac3e4 + iVar5) & 0x1000000) != 0)) &&
       (fVar7 - (float10)-60.0 <= (float10)*(int *)(&DAT_007ac3b4 + iVar5))) {
      *param_4 = 0xffffffff;
    }
    FUN_004226e9();
    FUN_0041e2b8();
  }
  else {
    FUN_004226e9();
    FUN_0041e2b8();
    *param_5 = 0;
    param_5[1] = 0;
  }
  *param_3 = 1;
  local_74 = local_78;
LAB_00474025:
  if (-1 < local_74) {
    *(undefined4 *)(&DAT_007ac3f8 + iVar5) = 0xaaa;
  }
  if ((((&DAT_007ac3e8)[param_1 * 0x14a] & 0x800) != 0) && (local_74 < 0)) {
    local_74 = local_64;
  }
  *(int *)(&DAT_007ac460 + iVar5) = local_74;
  return 3;
}


