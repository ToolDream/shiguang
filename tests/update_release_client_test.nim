## Called by update_release_test.sh against real pack/sign output.
import std/[base64, os, strutils]
import ../src/update_package

doAssert paramCount() == 4
let note = readFile(paramStr(2)).splitWhitespace()
doAssert note.len == 5 and note[0] == "<!--" and note[1] == "shiguang-update-v2" and note[4] == "-->"
let signed = verifyManifest(paramStr(1), decode(note[2]), note[3])
doAssert signed.releaseTag == "dev-latest"
doAssert signed.build.version == "dev-0123456789ab"
stagePackage(signed, readFile(paramStr(3)), paramStr(4), paramStr(1))
doAssert readFile(paramStr(4) / "shiguang-updater.exe") == "fixture helper"
echo "release ZIP and signature verified and staged by client"
let catalog = installedInventory(paramStr(1), readFile(paramStr(4) / InventoryName),
  readFile(paramStr(4) / InventoryProof), signed.build)
doAssert catalog.len == 7
