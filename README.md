# Automated Sorting System – Interface Specification

Nov 11, 2026 · @Leeky

## 1. Overview

The system has 11 interfaces across three subsystems. Each interface is listed with its inputs, outputs, behaviour and the data types it uses.

| Subsystem | Role | Interfaces |
| --- | --- | --- |
| SCADA/HMI | Shows the process state, alarms and sorting results to the operator | 3 |
| Vision | Reads the part labels and, optionally, locates the part on the conveyor | 2 |
| Controller | Drives the robot arm and the conveyor, and stores the position config | 6 |

**Conventions**

- All 3D coordinates are in the world frame, in **mm**.
- All joint angles are in **degrees**.
- Conveyor distance is in **mm**.
- `void` means the interface takes no input or returns nothing.
- String enum values are case-sensitive and must match this spec exactly.

## 2. SCADA/HMI interfaces

### 2.1 report current status

| Field | Value |
| --- | --- |
| Input | `status` |
| Output | `void` |
| Behaviour | Reports which step of the sequence control the system is currently in. |

**`status`** (string)

| Value | Meaning |
| --- | --- |
| `"moving conveyor"` | A part is being carried in by the conveyor |
| `"read label"` | Reading the part's label information |
| `"generate trajectory"` | Working out the target position from the config |
| `"pick up"` | Picking up the part |
| `"place"` | Placing the part |
| `"idle"` | Running, but waiting with no task |
| `"stop"` | Machine is stopped |

### 2.2 report alarm

| Field | Value |
| --- | --- |
| Input | `alarm message`, `alarm status` |
| Output | `void` |
| Behaviour | Reports the current state of one alarm. |

| Parameter | Type | Values | Meaning |
| --- | --- | --- | --- |
| `alarm message` | string | Free text | Describes the fault |
| `alarm status` | bool | `true` / `false` | Alarm raised / alarm reset |

### 2.3 report sorting result

| Field | Value |
| --- | --- |
| Input | `reading result`, `origin`, `destination`, `part ID`, `repository#`, `sorting result` |
| Output | `void` |
| Behaviour | Reports the final outcome for one part. |

The first four inputs are passed through from **read part label** (3.1) and use the types defined there.

| Parameter | Type | Values | Meaning |
| --- | --- | --- | --- |
| `repository#` | string | `"Repo<N>"` (e.g. `"Repo1"`) | The repository the part was placed in |
|  |  | `"manual"` | The part was not handled by the arm and was left for manual handling |
| `sorting result` | bool | `true` | Part was placed successfully |
|  |  | `false` | Part could not be placed (machine stopped, manual handling required) |

## 3. Vision subsystem interfaces

Each part has three labels: **Origin**, **Destination** and **Part ID**. The Part ID label carries a barcode.

### 3.1 read part label

| Field | Value |
| --- | --- |
| Input | `void` |
| Output | `reading result`, `origin`, `destination`, `part ID` |
| Behaviour | Uses the camera and OCR to read the English text on the Origin and Destination labels, and scans the barcode on the Part ID label. |

**`reading result`** (string)

| Value | Meaning |
| --- | --- |
| `"success"` | All three labels were read normally |
| `"info lost"` | At least one field is blank or one label was not found, but not all three |
| `"none"` | No part present: none of the three labels was found |
| `"fault"` | The vision system itself failed (e.g. camera not responding or OCR/barcode error), so no reading could be made |

**`origin` / `destination`** (string)

| Value | Meaning |
| --- | --- |
| City name in English (e.g. `"Toronto"`) | Read normally |
| `"none"` | The label was found but the field is blank |
| `"fault"` | The label was not found |

**`part ID`** (string)

| Value | Meaning |
| --- | --- |
| Integer in the int32 range, as a string (e.g. `"10234"`) | Read normally |
| `"none"` | The label was found but the field is blank |
| `"fault"` | The label was not found |

### 3.2 get part position (optional)

| Field | Value |
| --- | --- |
| Input | `void` |
| Output | `X`, `Y`, `Z` (mm) |
| Behaviour | Returns the world-frame coordinates of the part on the conveyor. |

## 4. Controller interfaces

### 4.1 update position config

| Field | Value |
| --- | --- |
| Input | N × (`repository#` → (X, Y, Z)); N × (`destination` → `repository#`); `part dock` → (X, Y, Z) |
| Output | `operation done` (bool): `true` = saved, `false` = failed |
| Behaviour | Saves the world-frame coordinates of each repository and of the part dock, plus the destination-to-repository map, to a local cache. The arm uses these later to find its target positions. |

See section 5 for an example config.

### 4.2 set arm position

| Field | Value |
| --- | --- |
| Input | `X`, `Y`, `Z` (mm) |
| Output | `operation result` |
| Behaviour | Solves inverse kinematics and moves the arm TCP to the given world-frame coordinates. |

| `operation result` (string) | Meaning |
| --- | --- |
| `"success"` | A solution exists and the TCP reached the position |
| `"invalid"` | No IK solution exists for the position |
| `"rejected"` | Another command is still running |

### 4.3 set joint position

| Field | Value |
| --- | --- |
| Input | `joint number`, `angle` (degrees) |
| Output | `operation result` |
| Behaviour | Rotates the given joint to the given angle. |

| `operation result` (string) | Meaning |
| --- | --- |
| `"success"` | The angle is within the joint limits and the joint reached it |
| `"invalid"` | The angle is outside the joint limits |
| `"rejected"` | Another command is still running |

### 4.4 move conveyor

| Field | Value |
| --- | --- |
| Input | `direction` (bool), `distance` (int, mm) |
| Output | `operation result` |
| Behaviour | Moves the conveyor belt the given distance in the given direction. |

| `operation result` (string) | Meaning |
| --- | --- |
| `"success"` | The conveyor moved the full requested distance |
| `"rejected"` | Another command is still running |

### 4.5 set running permit

| Field | Value |
| --- | --- |
| Input | `permit` (bool): `true` = start, `false` = stop |
| Output | `void` |
| Behaviour | Starts or stops the machine. |

### 4.6 get current arm position

| Field | Value |
| --- | --- |
| Input | `void` |
| Output | `X`, `Y`, `Z` (mm) |
| Behaviour | Computes forward kinematics from the current joint angles and returns the world-frame coordinates of the arm TCP. |

## 5. Example position config

| Repository / point | X (mm) | Y (mm) | Z (mm) | Destinations mapped to it |
| --- | --- | --- | --- | --- |
| Repo1 | 50 | 50 | 0 | Hamilton, Burlington |
| Repo2 | 100 | 50 | 0 | Mississauga, Oakville |
| Repo3 | 50 | 100 | 0 | Toronto |
| Repo4 | 100 | 100 | 0 | `"none"` (blank destination) |
| Part dock | 10 | 10 | 10 | — |

Destination names in the config must use the same spelling as the label text that OCR returns.

## 
