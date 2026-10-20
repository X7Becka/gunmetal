// 0x4799b0 - event queue drain/type switch (proven)
// binary: /gundx.exe-902b77
// ghidra: http://127.0.0.1:8765/mcp


/* event_dispatch: drains queued records and switches on the type byte. Type 1/3 -> generate_event
   (0x479960), type 2 -> FUN_0044f960, types 4..12 -> FUN_00463ab0. */

void event_dispatch(void)

{
  char *pcVar1;
  char cVar2;
  uint uVar3;
  int iVar4;
  int iVar5;
  undefined4 *puVar6;
  undefined4 *puVar7;
  
  iVar4 = 0;
  if ((DAT_0083d620 == 2) || (DAT_0083d620 == 3)) {
    DAT_0079cf6c = DAT_0062fbc0;
    DAT_007ab9f0 = DAT_0062fbc0;
    if (0 < (int)DAT_0062fbc0) {
      puVar6 = (undefined4 *)&DAT_0062fbc8;
      puVar7 = (undefined4 *)&DAT_0079cf80;
      for (uVar3 = DAT_0062fbc0 >> 2; uVar3 != 0; uVar3 = uVar3 - 1) {
        *puVar7 = *puVar6;
        puVar6 = puVar6 + 1;
        puVar7 = puVar7 + 1;
      }
      for (uVar3 = DAT_0062fbc0 & 3; uVar3 != 0; uVar3 = uVar3 - 1) {
        *(undefined1 *)puVar7 = *(undefined1 *)puVar6;
        puVar6 = (undefined4 *)((int)puVar6 + 1);
        puVar7 = (undefined4 *)((int)puVar7 + 1);
      }
      puVar6 = (undefined4 *)&DAT_0062fbc8;
      puVar7 = &DAT_007a44c0;
      for (uVar3 = DAT_0062fbc0 >> 2; uVar3 != 0; uVar3 = uVar3 - 1) {
        *puVar7 = *puVar6;
        puVar6 = puVar6 + 1;
        puVar7 = puVar7 + 1;
      }
      for (uVar3 = DAT_0062fbc0 & 3; uVar3 != 0; uVar3 = uVar3 - 1) {
        *(undefined1 *)puVar7 = *(undefined1 *)puVar6;
        puVar6 = (undefined4 *)((int)puVar6 + 1);
        puVar7 = (undefined4 *)((int)puVar7 + 1);
      }
    }
    DAT_0062fbc0 = 0;
  }
  if ((int)DAT_0079cf6c < 1) {
    DAT_0079cf6c = 0;
    return;
  }
  do {
    switch((&DAT_0079cf80)[iVar4]) {
    case 1:
      generate_event(*(undefined4 *)((int)&DAT_0079cf81 + iVar4),
                     (int)*(short *)(&DAT_0079cf85 + iVar4),*(undefined2 *)(&DAT_0079cf87 + iVar4),
                     *(undefined2 *)(&DAT_0079cf89 + iVar4),*(undefined4 *)(&DAT_0079cf8b + iVar4),
                     *(undefined4 *)(&DAT_0079cf8f + iVar4),*(undefined4 *)(&DAT_0079cf93 + iVar4),
                     *(undefined4 *)(&DAT_0079cf97 + iVar4));
      iVar4 = iVar4 + 0x1b;
      goto LAB_00479d60;
    case 2:
      FUN_0044f960(*(undefined4 *)((int)&DAT_0079cf81 + iVar4),
                   (int)*(short *)(&DAT_0079cf85 + iVar4));
      iVar4 = iVar4 + 7;
      goto LAB_00479d60;
    case 3:
      generate_event(*(undefined4 *)((int)&DAT_0079cf81 + iVar4),
                     (int)*(short *)(&DAT_0079cf85 + iVar4),0,0,
                     *(undefined4 *)(&DAT_0079cf87 + iVar4),*(undefined4 *)(&DAT_0079cf8b + iVar4),
                     *(undefined4 *)(&DAT_0079cf8f + iVar4),*(undefined4 *)(&DAT_0079cf93 + iVar4));
      iVar4 = iVar4 + 0x17;
      goto LAB_00479d60;
    case 4:
      FUN_00463ab0(iVar4 + 0x79cf82,*(undefined1 *)((int)&DAT_0079cf81 + iVar4),0xffffffff,
                   DAT_005a46e4 + 0x111);
      iVar5 = iVar4 + 3;
      cVar2 = *(char *)((int)&DAT_0079cf81 + iVar4 + 1);
      while (cVar2 != '\0') {
        pcVar1 = &DAT_0079cf80 + iVar5;
        iVar5 = iVar5 + 1;
        cVar2 = *pcVar1;
      }
      break;
    case 5:
      FUN_00463ab0(iVar4 + 0x79cf82,*(undefined1 *)((int)&DAT_0079cf81 + iVar4),0,
                   DAT_005a46e4 + 0x111);
      iVar5 = iVar4 + 3;
      cVar2 = *(char *)((int)&DAT_0079cf81 + iVar4 + 1);
      while (cVar2 != '\0') {
        pcVar1 = &DAT_0079cf80 + iVar5;
        iVar5 = iVar5 + 1;
        cVar2 = *pcVar1;
      }
      break;
    case 6:
      FUN_00463ab0(iVar4 + 0x79cf82,*(undefined1 *)((int)&DAT_0079cf81 + iVar4),1,
                   DAT_005a46e4 + 0x111);
      iVar5 = iVar4 + 3;
      cVar2 = *(char *)((int)&DAT_0079cf81 + iVar4 + 1);
      while (cVar2 != '\0') {
        pcVar1 = &DAT_0079cf80 + iVar5;
        iVar5 = iVar5 + 1;
        cVar2 = *pcVar1;
      }
      break;
    case 7:
      FUN_00463ab0(iVar4 + 0x79cf82,*(undefined1 *)((int)&DAT_0079cf81 + iVar4),2,
                   DAT_005a46e4 + 0x111);
      iVar5 = iVar4 + 3;
      cVar2 = *(char *)((int)&DAT_0079cf81 + iVar4 + 1);
      while (cVar2 != '\0') {
        pcVar1 = &DAT_0079cf80 + iVar5;
        iVar5 = iVar5 + 1;
        cVar2 = *pcVar1;
      }
      break;
    case 8:
      FUN_00463ab0(iVar4 + 0x79cf82,*(undefined1 *)((int)&DAT_0079cf81 + iVar4),3,
                   DAT_005a46e4 + 0x111);
      iVar5 = iVar4 + 3;
      cVar2 = *(char *)((int)&DAT_0079cf81 + iVar4 + 1);
      while (cVar2 != '\0') {
        pcVar1 = &DAT_0079cf80 + iVar5;
        iVar5 = iVar5 + 1;
        cVar2 = *pcVar1;
      }
      break;
    case 9:
      FUN_00463ab0(iVar4 + 0x79cf82,*(undefined1 *)((int)&DAT_0079cf81 + iVar4),4,
                   DAT_005a46e4 + 0x111);
      iVar5 = iVar4 + 3;
      cVar2 = *(char *)((int)&DAT_0079cf81 + iVar4 + 1);
      while (cVar2 != '\0') {
        pcVar1 = &DAT_0079cf80 + iVar5;
        iVar5 = iVar5 + 1;
        cVar2 = *pcVar1;
      }
      break;
    case 10:
      FUN_00463ab0(iVar4 + 0x79cf82,*(undefined1 *)((int)&DAT_0079cf81 + iVar4),5,
                   DAT_005a46e4 + 0x111);
      iVar5 = iVar4 + 3;
      cVar2 = *(char *)((int)&DAT_0079cf81 + iVar4 + 1);
      while (cVar2 != '\0') {
        pcVar1 = &DAT_0079cf80 + iVar5;
        iVar5 = iVar5 + 1;
        cVar2 = *pcVar1;
      }
      break;
    case 0xb:
      FUN_00463ab0(iVar4 + 0x79cf82,*(undefined1 *)((int)&DAT_0079cf81 + iVar4),6,
                   DAT_005a46e4 + 0x111);
      iVar5 = iVar4 + 3;
      cVar2 = *(char *)((int)&DAT_0079cf81 + iVar4 + 1);
      while (cVar2 != '\0') {
        pcVar1 = &DAT_0079cf80 + iVar5;
        iVar5 = iVar5 + 1;
        cVar2 = *pcVar1;
      }
      break;
    case 0xc:
      FUN_00463ab0(iVar4 + 0x79cf82,*(undefined1 *)((int)&DAT_0079cf81 + iVar4),7,
                   DAT_005a46e4 + 0x111);
      iVar5 = iVar4 + 3;
      cVar2 = *(char *)((int)&DAT_0079cf81 + iVar4 + 1);
      while (cVar2 != '\0') {
        pcVar1 = &DAT_0079cf80 + iVar5;
        iVar5 = iVar5 + 1;
        cVar2 = *pcVar1;
      }
      break;
    default:
      FUN_0040c530(s_Internal_Error__Bad_event_value_i_004bb148,(&DAT_0079cf80)[iVar4]);
      DAT_0079cf6c = 0;
      return;
    }
    iVar4 = iVar5 + 1;
LAB_00479d60:
    if ((int)DAT_0079cf6c <= iVar4) {
      DAT_0079cf6c = 0;
      return;
    }
  } while( true );
}


