# C&C3 game mechanics worth knowing when modding

Domain facts about how Tiberium Wars actually plays, which are not derivable
from the binary and which have already changed engineering conclusions.

## Repair drones

Every **vehicle and aircraft factory** fields repair drones that repair nearby
units of the same type. They are separate Objects, slaved to the factory.

Why it matters: a factory probes as having 3 "slaves", which looks exactly like
a Nod turret hub with 3 turrets. Without knowing about the drones, the obvious
reading is that the structure is a turret hub - that conclusion was drawn once
and was wrong. A slave can be a **mobile unit**, not just a static turret.

## Nod base defences

Nod builds defences as a **hub plus 3 expansion turrets**. The turrets are
separate Objects slaved to the hub, so anything that acts on "the structure"
has to decide whether it means the hub alone or the cluster.

## Selection

The game allows selecting only **one** enemy unit or structure at a time. Own
units can be multi-selected.

## Capturing satisfies objectives

In campaign, taking ownership of a structure counts as satisfying a "destroy"
objective for it.

## Garrison

Infantry entering a building disappear from view - but stay in the engine's
object list, still walkable - and the building's ownership follows its
occupants. Faction-specific window slits are rebuilt only
on the *entry* path, never on an ownership change.

## Engineer capture takes what is docked

Capturing a **refinery** with an engineer also gives you the harvester docked in
it; capturing an **air tower or airfield** gives you the aircraft parked on it.
Units that are away (a harvester in the field, aircraft in the air) stay with
the enemy.

Why it matters: the engine can tell docked from away, yet stores no link that
says so - six memory probes found nothing. The answer was to hook the engine
while it performed this capture. This observation, from the user, is what
solved parked-aircraft transfers.

## Transports

The GDI Ox, Nod Carryall and GDI MRT carry infantry squads (and the Carryall a vehicle)
in a `HordeTransportContain`. In the Firestorm mod, several campaign buildings use the
same container. Capturing one the way an engineer does evicts the passengers
immediately - like a garrison. The MRT is GDI's **APC**: ground transports and
transport aircraft share the container class (confirmed from live contain reports,
2026-09-19), so logic keyed on the class covers both.

## Campaign garrisons are often one scripted team

In at least one GDI mission, three enemy garrisons in a row, and every soldier inside
them, sat on the **same Team**. Taking one of them with F11 made the other two empty
out, although the tool's log shows it touched only the selected building and its
occupants. The likely cause is the mission script (or its team AI) reacting to losing a
member. Not confirmed by an engineer-capture control run. Skirmish garrisons do not do this.

## Aircraft return-to-base ability

Every aircraft that parks at an air tower or airfield has an ability that sends
it back to park. In the binary this is `ReturnToDockSpecialPower` - landing is
modelled as **docking**, like a harvester at a refinery.

## Capture announcements

The game's own voice line announces "Building captured" on any ownership change
of a building, including ones AllianceSwitchTool makes. Units change hands
silently.

See also `AllianceSwitchTool_Journey.md` (this folder) for how each of these was established.
