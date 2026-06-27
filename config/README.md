# NxFrame configuration layer

The `config/` folder contains preset/configuration validation code and example device configuration files. This layer should fail early with clear messages before NxFrame opens DeckLink devices, encoders, muxers, or network transports.

## File-by-file

### `preset_validator.cpp` / `preset_validator.h`

Validates sender and receiver JSON presets before runtime startup.

Responsibilities include:

- checking required top-level sections;
- enforcing expected JSON types;
- validating numeric ranges;
- validating known string enums;
- collecting all errors and warnings into a single result;
- printing stable diagnostics for release testing and field support.

The validator does not create encoders or receivers. It only checks whether the preset is structurally safe enough to continue startup.

### `system.example.json`

Example appliance-level system configuration. It documents a possible future/local device layout with control-network settings, streaming-network settings, and SDI port roles.

This file is an example only. Runtime-local `system.json` files should normally stay outside source control because they can contain machine-specific network addresses, device indexes, and routing choices.
