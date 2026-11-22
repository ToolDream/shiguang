import std/[os, tempfiles, unittest]
import ../src/[update_package, update_policy]
import update_package_fixture

suite "signed update package":
  test "known signature validates only its exact message and configured public key":
    # RFC 6979 A.2.5 P-256 SHA-256 test vector, independently generated.
    let key = "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6" &
              "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299"
    let sig = "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716" &
              "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"
    check verifyUpdateSignature(key, "sample", sig)
    check not verifyUpdateSignature(key, "Sample", sig)
    check not verifyUpdateSignature("", "sample", sig)

  test "signed manifest is bound to repository, architecture, path and size":
    let good = verifyManifest(PublicKey, GoodManifest, GoodSignature)
    check good.build.order == 42
    check good.build.channel == ucStable
    check good.packageUrl == "https://github.com/ToolDream/shiguang/releases/download/v1.2.3/shiguang-windows-x64.zip"
    expect ValueError:
      discard verifyManifest(PublicKey, GoodManifest & "\n", GoodSignature)
    expect ValueError:
      discard verifyManifest("", GoodManifest, GoodSignature)
    for fixture in [(WrongRepo, WrongRepoSig), (WrongArch, WrongArchSig),
                    (Oversized, OversizedSig), (WrongPath, WrongPathSig),
                    (WrongTag, WrongTagSig), (Duplicate, DuplicateSig)]:
      expect ValueError:
        discard verifyManifest(PublicKey, fixture[0], fixture[1])

  test "invalid archive bytes, entries and interrupted download never stage":
    let dir = createTempDir("sg-package-test-", "")
    defer: removeDir(dir)
    let good = verifyManifest(PublicKey, GoodManifest, GoodSignature)
    let stage = dir / "stage"
    for input in [GoodZip[0 ..< GoodZip.high], GoodZip & "x", "x" & GoodZip[1 .. ^1]]:
      expect ValueError:
        stagePackage(good, input, stage)
      check not dirExists(stage)
    for fixture in [(LinkManifest, LinkSignature, LinkZip),
                    (ExtraManifest, ExtraSignature, ExtraZip),
                    (FileHashManifest, FileHashSignature, GoodZip)]:
      let bad = verifyManifest(PublicKey, fixture[0], fixture[1])
      expect ValueError:
        stagePackage(bad, fixture[2], stage)
      check not dirExists(stage)

  test "checked stage contains only managed entries and cannot overwrite an existing stage":
    let dir = createTempDir("sg-package-test-", "")
    defer: removeDir(dir)
    let good = verifyManifest(PublicKey, GoodManifest, GoodSignature)
    let stage = dir / "stage"
    stagePackage(good, GoodZip, stage)
    check readFile(stage / "shiguang.exe") == "app"
    check readFile(stage / "runtime.dll") == "dll"
    check readFile(stage / "BUILD-MANIFEST.json") == "{}"
    expect ValueError:
      stagePackage(good, GoodZip, stage)
    check readFile(stage / "shiguang.exe") == "app"

  test "an outer ZIP signature cannot authorize an embedded inventory":
    expect ValueError:
      discard verifyInventory(PublicKey, GoodManifest, GoodSignature)

  test "rolling test identity is distinct from its fixed asset tag and inventory must match":
    let m = verifyManifest(RollingKey, RollingBManifest, RollingBSignature)
    check m.build.version == "dev-0123456789ab"
    check m.releaseTag == "dev-latest"
    let mixed = verifyManifest(RollingKey, RollingMixedManifest, RollingMixedSig)
    let dir = createTempDir("sg-mixed-archive-", "")
    defer: removeDir(dir)
    let stage = dir / "stage"
    expect ValueError:
      stagePackage(m, RollingAZip, stage, RollingKey)
    expect ValueError:
      stagePackage(mixed, RollingMixedZip, stage, RollingKey)
    check not dirExists(stage)
    stagePackage(m, RollingBZip, stage, RollingKey)
    check readFile(stage / InventoryName) == RollingBInventory
