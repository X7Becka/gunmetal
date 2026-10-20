// 0x41e025 - camera basis Rz(-yaw)*Rx(-pitch)*Ry(roll) (proven)
// binary: /gundx.exe-902b77
// ghidra: http://127.0.0.1:8765/mcp


void FUN_0041e025(double *param_1,double *param_2,double *param_3,undefined4 param_4,
                 undefined4 param_5,undefined4 param_6,undefined4 param_7,undefined4 param_8,
                 undefined4 param_9)

{
  double dVar1;
  double dVar2;
  double dVar3;
  double dVar4;
  double dVar5;
  double dVar6;
  float10 fVar7;
  
  fVar7 = (float10)FUN_00480314(param_4,param_5);
  dVar1 = (double)fVar7;
  fVar7 = (float10)FUN_00480954(param_4,param_5);
  dVar2 = (double)fVar7;
  fVar7 = (float10)FUN_00480314(param_6,param_7);
  dVar3 = (double)fVar7;
  fVar7 = (float10)FUN_00480954(param_6,param_7);
  dVar4 = (double)fVar7;
  fVar7 = (float10)FUN_00480314(param_8,param_9);
  dVar5 = (double)fVar7;
  fVar7 = (float10)FUN_00480954(param_8,param_9);
  dVar6 = (double)fVar7;
  *param_1 = dVar6 * dVar2 - dVar5 * dVar3 * dVar1;
  param_1[1] = -dVar5 * dVar2 - dVar6 * dVar3 * dVar1;
  param_1[2] = -dVar4 * dVar1;
  *param_2 = dVar5 * dVar4;
  param_2[1] = dVar6 * dVar4;
  param_2[2] = -dVar3;
  *param_3 = dVar5 * dVar3 * dVar2 + dVar6 * dVar1;
  param_3[1] = dVar6 * dVar3 * dVar2 + -dVar5 * dVar1;
  param_3[2] = dVar4 * dVar2;
  return;
}


