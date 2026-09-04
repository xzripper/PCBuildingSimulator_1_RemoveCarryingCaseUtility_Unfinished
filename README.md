# PCBuildingSimulator_1_RemoveCarryingCaseUtility_Unfinished
Unifinished utility for PC Building Simulator 1 to remove carrying case in hands in Free Build mode.

The chain of retrieving the case for removal: `WorkshopController` -> `m_carryingCase` -> `class Case (0x...)` (Found out via dnSpy and Cheat Engine)

Removes the case by pressing F5.

1. Causes deadlock after closing the game, after injection.
2. No final code of the injector.
3. Needs finalization.
