.. SPDX-License-Identifier: GPL-2.0

===========
DSA in ACPI
===========

The **Distributed Switch Architecture (DSA)** devices on an MDIO bus [dsa]
are enumerated using fwnode_mdiobus_register_device() and later probed
by a dedicated driver based on the ACPI ID match result.

In DSDT/SSDT the scope of switch device is extended by the front-panel
and one or more so called 'CPU' switch ports. Additionally
subsequent MDIO busses with attached PHYs can be described.

This document presents the switch description with the required subnodes
and _DSD properties.

These properties are defined in accordance with the "Device
Properties UUID For _DSD" [dsd-guide] document and the
daffd814-6eba-4d8c-8a91-bc9bbf4aa301 UUID must be used in the Device
Data Descriptors containing them.

Switch device
=============

The switch device is represented as a child node of the MDIO bus.
It must comprise the _HID (and optionally _CID) field, so to allow matching
with appropriate driver via ACPI ID. The other obligatory field is
_ADR with the device address on the MDIO bus [adr]. Below example
shows 'SWI0' switch device at address 0x4 on the 'SMI0' bus.

.. code-block:: none

    Scope (\_SB.SMI0)
    {
        Name (_HID, "MRVL0100")
        Name (_UID, 0x00)
        Method (_STA)
        {
            Return (0xF)
        }
        Name (_CRS, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite,
                0xf212a200,
                0x00000010,
                )
        })
        Device (SWI0)
        {
            Name (_HID, "MRVL0120")
            Name (_UID, 0x00)
            Name (_ADR, 0x4)
            <...>
        }
    }

Switch MDIO bus
===============

A switch internal MDIO bus, please refer to 'MDIO bus and PHYs in ACPI' [phy]
document for more details. Its name must be set to **MDIO** for proper
enumeration by net/dsa API.

Switch MDIO bus declaration example:
------------------------------------

.. code-block:: none

    Scope (\_SB.SMI0.SWI0)
    {
        Name (_HID, "MRVL0120")
        Name (_UID, 0x00)
        Name (_ADR, 0x4)
        Device (MDIO) {
            Name (_ADR, 0x0)
            Device (S0P0)
            {
                Name (_ADR, 0x11)
            }
            Device (S0P1)
            {
                Name (_ADR, 0x12)
            }
            Device (S0P2)
            {
                Name (_ADR, 0x13)
            }
            Device (S0P3)
            {
                Name (_ADR, 0x14)
            }
        }
        <...>
    }

Switch ports
============

The ports must be grouped under **PRTS** switch child device. They
should comprise a _ADR field with a port enumerator [adr] and
other properties in a standard _DSD object [dsa-properties].

label
-----
A property with a string value describing port's name in the OS. In case the
port is connected to the MAC ('CPU' port), its value should be set to "cpu".

phy-handle
----------
For each MAC node, a device property "phy-handle" is used to reference
the PHY that is registered on an MDIO bus. This is mandatory for
network interfaces that have PHYs connected to MAC via MDIO bus.
See [phy] for more details.

ethernet
--------
A property valid for the so called 'CPU' port and should comprise a reference
to the MAC object declared in the DSDT/SSDT.

fixed-link
----------
The 'fixed-link' is described by a data-only subnode of the
port, which is linked in the _DSD package via
hierarchical data extension (UUID dbb8e3e6-5886-4ba6-8795-1319f52a966b
in accordance with [dsd-guide] "_DSD Implementation Guide" document).
The subnode should comprise a required property ("speed") and
possibly the optional ones - complete list of parameters and
their values are specified in [ethernet-controller].
See [phy] for more details.

Switch ports' description example:
----------------------------------

.. code-block:: none

    Scope (\_SB.SMI0.SWI0)
    {
        Name (_HID, "MRVL0120")
        Name (_UID, 0x00)
        Name (_ADR, 0x4)
        Device (PRTS) {
            Name (_ADR, 0x0)
            Device (PRT1)
            {
                Name (_ADR, 0x1)
                Name (_DSD, Package () {
                    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                    Package () {
                      Package () { "label", "lan2"},
                      Package () { "phy-handle", \_SB.SMI0.SWI0.MDIO.S0P0},
                    }
                })
            }
            Device (PRT2)
            {
                Name (_ADR, 0x2)
                Name (_DSD, Package () {
                    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                    Package () {
                      Package () { "label", "lan1"},
                    },
                    ToUUID("dbb8e3e6-5886-4ba6-8795-1319f52a966b"),
                    Package () {
                      Package () {"fixed-link", "LNK0"}
                    }
                })
                Name (LNK0, Package(){ // Data-only subnode of port
                    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                    Package () {
                      Package () {"speed", 1000},
                      Package () {"full-duplex", 1}
                    }
                })
            }
            Device (PRT3)
            {
                Name (_ADR, 0x3)
                Name (_DSD, Package () {
                    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                    Package () {
                      Package () { "label", "lan4"},
                      Package () { "phy-handle", \_SB.SMI0.SWI0.MDIO.S0P2},
                    }
                })
            }
            Device (PRT4)
            {
                Name (_ADR, 0x4)
                Name (_DSD, Package () {
                    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                    Package () {
                      Package () { "label", "lan3"},
                      Package () { "phy-handle", \_SB.SMI0.SWI0.MDIO.S0P3},
                    }
                })
            }
            Device (PRT5)
            {
                Name (_ADR, 0x5)
                Name (_DSD, Package () {
                    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                    Package () {
                      Package () { "label", "cpu"},
                      Package () { "ethernet", \_SB.PP20.ETH2},
                    }
                })
            }
        }
        <...>
    }

Full DSA description example
============================

Below example comprises MDIO bus ('SMI0') with a PHY at address 0x0 ('PHY0')
and a switch ('SWI0') at 0x4. The so called 'CPU' port ('PRT5') is connected to
the SoC's MAC (\_SB.PP20.ETH2). 'PRT2' port is configured as 1G fixed-link.

.. code-block:: none

    Scope (\_SB.SMI0)
    {
        Name (_HID, "MRVL0100")
        Name (_UID, 0x00)
        Method (_STA)
        {
            Return (0xF)
        }
        Name (_CRS, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite,
                0xf212a200,
                0x00000010,
                )
        })
        Device (PHY0)
        {
            Name (_ADR, 0x0)
        }
        Device (SWI0)
        {
            Name (_HID, "MRVL0120")
            Name (_UID, 0x00)
            Name (_ADR, 0x4)
            Device (PRTS) {
                Name (_ADR, 0x0)
                Device (PRT1)
                {
                    Name (_ADR, 0x1)
                    Name (_DSD, Package () {
                        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                        Package () {
                          Package () { "label", "lan2"},
                          Package () { "phy-handle", \_SB.SMI0.SWI0.MDIO.S0P0},
                        }
                    })
                }
                Device (PRT2)
                {
                    Name (_ADR, 0x2)
                    Name (_DSD, Package () {
                        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                        Package () {
                          Package () { "label", "lan1"},
                        },
                        ToUUID("dbb8e3e6-5886-4ba6-8795-1319f52a966b"),
                        Package () {
                          Package () {"fixed-link", "LNK0"}
                        }
                    })
                    Name (LNK0, Package(){ // Data-only subnode of port
                        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                        Package () {
                          Package () {"speed", 1000},
                          Package () {"full-duplex", 1}
                        }
                    })
                }
                Device (PRT3)
                {
                    Name (_ADR, 0x3)
                    Name (_DSD, Package () {
                        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                        Package () {
                          Package () { "label", "lan4"},
                          Package () { "phy-handle", \_SB.SMI0.SWI0.MDIO.S0P2},
                        }
                    })
                }
                Device (PRT4)
                {
                    Name (_ADR, 0x4)
                    Name (_DSD, Package () {
                        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                        Package () {
                          Package () { "label", "lan3"},
                          Package () { "phy-handle", \_SB.SMI0.SWI0.MDIO.S0P3},
                        }
                    })
                }
                Device (PRT5)
                {
                    Name (_ADR, 0x5)
                    Name (_DSD, Package () {
                        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                        Package () {
                          Package () { "label", "cpu"},
                          Package () { "ethernet", \_SB.PP20.ETH2},
                        }
                    })
                }
            }
            Device (MDIO) {
                Name (_ADR, 0x0)
                Device (S0P0)
                {
                    Name (_ADR, 0x11)
                }
                Device (S0P2)
                {
                    Name (_ADR, 0x13)
                }
                Device (S0P3)
                {
                    Name (_ADR, 0x14)
                }
            }
        }
    }

TODO
====

* Add support for cascade switch connections via port's 'link' property [dsa-properties].

References
==========

[adr] ACPI Specifications, Version 6.4 - Paragraph 6.1.1 _ADR Address
    https://uefi.org/specs/ACPI/6.4/06_Device_Configuration/Device_Configuration.html#adr-address

[dsa]
    Documentation/networking/dsa/dsa.rst

[dsa-properties]
    Documentation/devicetree/bindings/net/dsa/dsa-port.yaml

[dsd-guide] DSD Guide.
    https://github.com/UEFI/DSD-Guide/blob/main/dsd-guide.adoc, referenced
    2022-06-20.

[dsd-properties-rules]
    Documentation/firmware-guide/acpi/DSD-properties-rules.rst

[ethernet-controller]
    Documentation/devicetree/bindings/net/ethernet-controller.yaml

[phy] Documentation/networking/phy.rst
