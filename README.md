# MTG Commander (EDH) Life & Damage Tracker for Flipper Zero

A full-featured 4-Player Magic: The Gathering Commander (EDH) life and counter tracker built for the Flipper Zero's 128x64 display.

---

## Features

- **4-Player Quad Overview**: High-density 2x2 grid displaying all 4 players (P1, P2, P3, P4) with large, readable life numbers. Clean quadrants with dynamic counter badges only when active.
- **Opponent-Specific Commander Damage Tracking**:
  - In-depth tracking of combat damage received from each opponent's commander (e.g. P1 tracks CD from P2, P3, and P4).
  - Automatically adjusts regular life total when commander damage is dealt (per MTG combat rules).
  - Triggers lethal alert & double vibration when reaching **21 Commander Damage**.
- **Essential Counters**:
  - **Poison / Infect Counters**: Lethal alert at **10 Poison**.
  - **Commander Tax Tracker**: Easily track additional casting cost (+2, +4, +6, etc.).
- **Tournament & Table Utilities**:
  - **D20 Die Roller**: Displays roll result with *Natural 20* and *Critical Fail* callouts.
  - **D6 Die Roller**: Six-sided die roller with centered die box.
  - **Coin Flip**: High-visibility coin flip (Heads or Tails).
  - **Pick First Player**: Automatically decides who takes turn 1 among players 1-4.
  - **Custom Starting Life**: Configurable between 40 (standard Commander), 30, or 20 life.
- **SD Card Auto-Save**: Saves game state to `/ext/apps_data/mtg_commander/save.bin` so games survive battery sleep and app exits.
- **Haptic & Audio Feedback**: Tactile clicks on adjustments and distinct alert vibrations on lethal damage or life reaching 0.

---

## Controls

### 1. Quad View (Main 4-Player Screen)
- **Left / Right**: Cycle selected player (P1 -> P2 -> P3 -> P4).
- **Up / Down**: ±1 Life (Hold Up/Down for ±5 Life).
- **OK (Short press)**: Open **Player Detail View** for the selected player.
- **OK (Long press)**: Open **Game Options Menu**.
- **Back**: Exit application (auto-saves state).

### 2. Player Detail View
- **Up / Down**: Move cursor between opponent Commander Damage rows, Commander Tax, and Poison counters.
- **Right / Left**: ±1 to the selected counter (increments/decrements Commander Damage and syncs Life).
- **OK**: Quick increment (+5 for CD, +2 for Tax/Poison).
- **Back**: Return to 4-Player Quad View.

### 3. Game Options Menu
1. **Pick First Player**: Random 1-4 chooser.
2. **Reset Game**: Reset all life totals to starting life and clear counters.
3. **Starting Life**: Toggle starting life between 40, 30, and 20.
4. **Clear Cmdr Damage**: Reset CD counters without altering life totals.
5. **Roll D20 Die**: Roll a random d20.
6. **Roll D6 Die**: Roll a random d6.
7. **Flip a Coin**: Random Heads / Tails coin flip.
8. **Vibration / Sound**: Toggle tactile and audio alerts.
9. **About & Help**: In-app instructions, rules, author credit, and GitHub link.
10. **Back to Game**: Return to Quad View.

---

## How to Install and Launch

### Option 1: Direct USB Launch with uFBT (Recommended)
1. Connect your Flipper Zero via USB.
2. Open PowerShell in this directory (`C:\Users\reube\Dev\flipper-mtg-commander`).
3. Run:
   ```powershell
   ufbt launch
   ```

### Option 2: Copy to MicroSD Card
Copy `dist/mtg_commander.fap` to your Flipper's microSD card under:
```
SD Card/apps/Tools/mtg_commander.fap
```
or use **qFlipper** to drag-and-drop it into the `apps/Tools` folder.
