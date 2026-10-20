// 0x433e00 - SOO/thing animation player (proven)
// binary: /gundx.exe-902b77
// ghidra: http://127.0.0.1:8765/mcp


void FUN_00433e00(int param_1,int param_2)

{
  int iVar1;
  float fVar2;
  undefined1 uVar3;
  undefined2 uVar4;
  undefined4 uVar5;
  int iVar6;
  uint uVar7;
  int iVar8;
  int iVar9;
  uint uVar10;
  int iVar11;
  float *pfVar12;
  float *pfVar13;
  float *pfVar14;
  undefined2 *puVar15;
  int iVar16;
  undefined2 *puVar17;
  int iVar18;
  int iVar19;
  int local_3c;
  int local_38;
  int local_28;
  int local_24;
  int local_1c;
  
  local_38 = 0;
  if (*(int *)(param_1 + 0x7c) < 0) {
    iVar11 = (-1 - *(int *)(param_1 + 0x7c)) * 0x108;
    for (local_1c = 0; local_1c < *(int *)(&DAT_006fbd44 + iVar11); local_1c = local_1c + 1) {
      local_38 = local_38 + *(int *)(iVar11 + 0x6fbdc8 + local_1c * 4);
    }
    if (0 < local_38) {
      local_3c = param_2 - *(int *)(param_1 + 0x78);
      if (local_3c < 0) {
        local_3c = 0;
      }
      iVar1 = *(int *)(&DAT_006fbd40 + iVar11);
      if (iVar1 == 0) {
        if (local_38 <= local_3c) {
          local_3c = local_38 + -1;
        }
      }
      else if (iVar1 == 1) {
        local_3c = local_3c % local_38;
      }
      local_1c = 0;
      for (local_28 = 0;
          (local_1c < *(int *)(&DAT_006fbd44 + iVar11) &&
          (local_28 + *(int *)(iVar11 + 0x6fbdc8 + local_1c * 4) <= local_3c));
          local_28 = local_28 + *(int *)(iVar11 + 0x6fbdc8 + iVar1)) {
        iVar1 = local_1c * 4;
        local_1c = local_1c + 1;
      }
      iVar1 = *(int *)(&DAT_006fbd48 + local_1c * 4 + iVar11);
      fVar2 = (float)(local_3c - local_28) / (float)*(int *)(iVar11 + 0x6fbdc8 + local_1c * 4);
      if (local_1c == *(int *)(&DAT_006fbd44 + iVar11) + -1) {
        local_24 = iVar1;
        if (*(int *)(&DAT_006fbd40 + iVar11) != 0) {
          local_24 = *(int *)(&DAT_006fbd48 + iVar11);
        }
      }
      else {
        local_24 = *(int *)(iVar11 + 0x6fbd4c + local_1c * 4);
      }
      for (local_1c = 0; local_1c < (int)(uint)*(ushort *)(param_1 + 0x68); local_1c = local_1c + 1)
      {
        pfVar12 = (float *)(*(int *)(iVar1 + 100) + local_1c * 0xc);
        pfVar13 = (float *)(*(int *)(local_24 + 100) + local_1c * 0xc);
        pfVar14 = (float *)(*(int *)(param_1 + 100) + local_1c * 0xc);
        *pfVar14 = (*pfVar13 - *pfVar12) * fVar2 + *pfVar12;
        pfVar14[1] = (pfVar13[1] - pfVar12[1]) * fVar2 + pfVar12[1];
        pfVar14[2] = (pfVar13[2] - pfVar12[2]) * fVar2 + pfVar12[2];
      }
      for (local_1c = 0; local_1c < (int)(uint)*(ushort *)(param_1 + 0x6a); local_1c = local_1c + 1)
      {
        puVar15 = (undefined2 *)(*(int *)(iVar1 + 0x6c) + local_1c * 0x80);
        iVar16 = *(int *)(local_24 + 0x6c) + local_1c * 0x80;
        puVar17 = (undefined2 *)(*(int *)(param_1 + 0x6c) + local_1c * 0x80);
        *(float *)(puVar17 + 4) =
             (*(float *)(iVar16 + 8) - *(float *)(puVar15 + 4)) * fVar2 + *(float *)(puVar15 + 4);
        *(float *)(puVar17 + 6) =
             (*(float *)(iVar16 + 0xc) - *(float *)(puVar15 + 6)) * fVar2 + *(float *)(puVar15 + 6);
        *(float *)(puVar17 + 8) =
             (*(float *)(iVar16 + 0x10) - *(float *)(puVar15 + 8)) * fVar2 + *(float *)(puVar15 + 8)
        ;
        *(float *)(puVar17 + 10) =
             (*(float *)(iVar16 + 0x14) - *(float *)(puVar15 + 10)) * fVar2 +
             *(float *)(puVar15 + 10);
        *(float *)(puVar17 + 0xc) =
             (*(float *)(iVar16 + 0x18) - *(float *)(puVar15 + 0xc)) * fVar2 +
             *(float *)(puVar15 + 0xc);
        *(float *)(puVar17 + 0xe) =
             (*(float *)(iVar16 + 0x1c) - *(float *)(puVar15 + 0xe)) * fVar2 +
             *(float *)(puVar15 + 0xe);
        *(float *)(puVar17 + 0x10) =
             (*(float *)(iVar16 + 0x20) - *(float *)(puVar15 + 0x10)) * fVar2 +
             *(float *)(puVar15 + 0x10);
        *(float *)(puVar17 + 0x12) =
             (*(float *)(iVar16 + 0x24) - *(float *)(puVar15 + 0x12)) * fVar2 +
             *(float *)(puVar15 + 0x12);
        *(float *)(puVar17 + 0x14) =
             (*(float *)(iVar16 + 0x28) - *(float *)(puVar15 + 0x14)) * fVar2 +
             *(float *)(puVar15 + 0x14);
        *(float *)(puVar17 + 0x16) =
             (*(float *)(iVar16 + 0x2c) - *(float *)(puVar15 + 0x16)) * fVar2 +
             *(float *)(puVar15 + 0x16);
        *(float *)(puVar17 + 0x18) =
             (*(float *)(iVar16 + 0x30) - *(float *)(puVar15 + 0x18)) * fVar2 +
             *(float *)(puVar15 + 0x18);
        *(float *)(puVar17 + 0x1a) =
             (*(float *)(iVar16 + 0x34) - *(float *)(puVar15 + 0x1a)) * fVar2 +
             *(float *)(puVar15 + 0x1a);
        uVar3 = __ftol();
        *(undefined1 *)(puVar17 + 0x38) = uVar3;
        *puVar17 = *puVar15;
        puVar17[2] = puVar15[2];
        puVar17[1] = puVar15[1];
        uVar4 = __ftol();
        puVar17[0x28] = uVar4;
        uVar4 = __ftol();
        puVar17[0x29] = uVar4;
        uVar5 = __ftol();
        *(undefined4 *)(puVar17 + 0x2a) = uVar5;
        uVar5 = __ftol();
        *(undefined4 *)(puVar17 + 0x2c) = uVar5;
        iVar11 = (int)(short)puVar15[0x2e];
        iVar18 = (int)*(short *)(iVar16 + 0x5c) - (int)(short)puVar15[0x2e];
        uVar4 = __ftol();
        puVar17[0x2e] = uVar4;
        iVar9 = (int)(short)puVar15[0x30];
        iVar6 = (int)*(short *)(iVar16 + 0x60) - (int)(short)puVar15[0x30];
        uVar4 = __ftol();
        puVar17[0x30] = uVar4;
        uVar7 = (uint)(ushort)puVar15[0x33];
        iVar19 = (uint)*(ushort *)(iVar16 + 0x66) - (uint)(ushort)puVar15[0x33];
        uVar4 = __ftol();
        puVar17[0x33] = uVar4;
        uVar10 = (uint)(ushort)puVar15[0x2f];
        iVar8 = (uint)*(ushort *)(iVar16 + 0x5e) - (uint)(ushort)puVar15[0x2f];
        uVar4 = __ftol();
        puVar17[0x2f] = uVar4;
        uVar4 = __ftol((uint)*(ushort *)(iVar16 + 0x62) - (uint)(ushort)puVar15[0x31],puVar15[0x31],
                       iVar8,uVar10,iVar19,uVar7,iVar6,iVar9,iVar18,iVar11);
        puVar17[0x31] = uVar4;
      }
      FUN_00434d78(param_1);
    }
  }
  return;
}


