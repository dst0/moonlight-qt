#include <QtTest>

#include "streaming/input/keymapping.h"
#include "tests.h"

namespace {

SDL_Keysym makeKeysym(SDL_Scancode scancode, SDL_Keycode sym, Uint16 mod)
{
    SDL_Keysym keysym;
    memset(&keysym, 0, sizeof(keysym));
    keysym.scancode = scancode;
    keysym.sym = sym;
    keysym.mod = mod;
    keysym.unused = 0xABCD1234;
    return keysym;
}

// Every modifier bit SDL2 defines
const Uint16 kAllModifierBits[] = {
    KMOD_LSHIFT, KMOD_RSHIFT, KMOD_LCTRL, KMOD_RCTRL, KMOD_LALT, KMOD_RALT,
    KMOD_LGUI, KMOD_RGUI, KMOD_NUM, KMOD_CAPS, KMOD_MODE, KMOD_SCROLL,
};

// Reference implementation of the modifier swap, written independently of the code under test
Uint16 expectedSwappedMod(Uint16 mod)
{
    Uint16 out = 0;
    for (Uint16 bit : kAllModifierBits) {
        if (!(mod & bit)) {
            continue;
        }
        switch (bit) {
        case KMOD_LCTRL: out |= KMOD_LGUI; break;
        case KMOD_RCTRL: out |= KMOD_RGUI; break;
        case KMOD_LGUI: out |= KMOD_LCTRL; break;
        case KMOD_RGUI: out |= KMOD_RCTRL; break;
        default: out |= bit; break;
        }
    }
    return out;
}

}

class TestKeyMapping : public QObject
{
    Q_OBJECT

private slots:
    void modifierKeysAreSwapped_data()
    {
        QTest::addColumn<int>("scancode");
        QTest::addColumn<int>("sym");
        QTest::addColumn<int>("expectedScancode");
        QTest::addColumn<int>("expectedSym");

        QTest::newRow("left Command -> left Control") << (int)SDL_SCANCODE_LGUI << (int)SDLK_LGUI
                                                      << (int)SDL_SCANCODE_LCTRL << (int)SDLK_LCTRL;
        QTest::newRow("left Control -> left Command") << (int)SDL_SCANCODE_LCTRL << (int)SDLK_LCTRL
                                                      << (int)SDL_SCANCODE_LGUI << (int)SDLK_LGUI;
        QTest::newRow("right Command -> right Control") << (int)SDL_SCANCODE_RGUI << (int)SDLK_RGUI
                                                        << (int)SDL_SCANCODE_RCTRL << (int)SDLK_RCTRL;
        QTest::newRow("right Control -> right Command") << (int)SDL_SCANCODE_RCTRL << (int)SDLK_RCTRL
                                                        << (int)SDL_SCANCODE_RGUI << (int)SDLK_RGUI;

        // Everything else is untouched
        QTest::newRow("C") << (int)SDL_SCANCODE_C << (int)SDLK_c << (int)SDL_SCANCODE_C << (int)SDLK_c;
        QTest::newRow("V") << (int)SDL_SCANCODE_V << (int)SDLK_v << (int)SDL_SCANCODE_V << (int)SDLK_v;
        QTest::newRow("left Alt") << (int)SDL_SCANCODE_LALT << (int)SDLK_LALT << (int)SDL_SCANCODE_LALT << (int)SDLK_LALT;
        QTest::newRow("right Alt") << (int)SDL_SCANCODE_RALT << (int)SDLK_RALT << (int)SDL_SCANCODE_RALT << (int)SDLK_RALT;
        QTest::newRow("left Shift") << (int)SDL_SCANCODE_LSHIFT << (int)SDLK_LSHIFT << (int)SDL_SCANCODE_LSHIFT << (int)SDLK_LSHIFT;
        QTest::newRow("Application") << (int)SDL_SCANCODE_APPLICATION << (int)SDLK_APPLICATION
                                     << (int)SDL_SCANCODE_APPLICATION << (int)SDLK_APPLICATION;
        QTest::newRow("Space") << (int)SDL_SCANCODE_SPACE << (int)SDLK_SPACE << (int)SDL_SCANCODE_SPACE << (int)SDLK_SPACE;
        // Mismatched scancode/keycode (non-QWERTY layout): each is mapped on its own
        QTest::newRow("GUI scancode, other keycode") << (int)SDL_SCANCODE_LGUI << (int)SDLK_a
                                                     << (int)SDL_SCANCODE_LCTRL << (int)SDLK_a;
    }

    void modifierKeysAreSwapped()
    {
        QFETCH(int, scancode);
        QFETCH(int, sym);
        QFETCH(int, expectedScancode);
        QFETCH(int, expectedSym);

        SDL_Keysym in = makeKeysym((SDL_Scancode)scancode, (SDL_Keycode)sym, KMOD_NONE);
        SDL_Keysym out = KeyMapping::applyCmdCtrlSwap(in, true);
        QCOMPARE((int)out.scancode, expectedScancode);
        QCOMPARE((int)out.sym, expectedSym);
        QCOMPARE(out.unused, in.unused);

        // Disabled: identity
        SDL_Keysym same = KeyMapping::applyCmdCtrlSwap(in, false);
        QCOMPARE((int)same.scancode, scancode);
        QCOMPARE((int)same.sym, sym);
    }

    void allModifierCombinations()
    {
        const int bitCount = sizeof(kAllModifierBits) / sizeof(kAllModifierBits[0]);
        for (int combo = 0; combo < (1 << bitCount); combo++) {
            Uint16 mod = 0;
            for (int i = 0; i < bitCount; i++) {
                if (combo & (1 << i)) {
                    mod |= kAllModifierBits[i];
                }
            }

            SDL_Keysym in = makeKeysym(SDL_SCANCODE_C, SDLK_c, mod);

            SDL_Keysym swapped = KeyMapping::applyCmdCtrlSwap(in, true);
            QCOMPARE(swapped.mod, expectedSwappedMod(mod));
            QCOMPARE((int)swapped.scancode, (int)SDL_SCANCODE_C);
            QCOMPARE((int)swapped.sym, (int)SDLK_c);

            // Swapping twice restores the original
            QCOMPARE(KeyMapping::applyCmdCtrlSwap(swapped, true).mod, mod);

            // Disabled: identity for every combination
            SDL_Keysym same = KeyMapping::applyCmdCtrlSwap(in, false);
            QCOMPARE(same.mod, mod);
            QCOMPARE((int)same.scancode, (int)in.scancode);
            QCOMPARE((int)same.sym, (int)in.sym);
            QCOMPARE(same.unused, in.unused);
        }
    }

    void commandCBecomesControlC()
    {
        // Cmd down, then C with Cmd held: the host must see Ctrl down and Ctrl+C
        SDL_Keysym cmdDown = KeyMapping::applyCmdCtrlSwap(makeKeysym(SDL_SCANCODE_LGUI, SDLK_LGUI, KMOD_LGUI), true);
        QCOMPARE((int)cmdDown.scancode, (int)SDL_SCANCODE_LCTRL);
        QCOMPARE(cmdDown.mod, (Uint16)KMOD_LCTRL);

        SDL_Keysym c = KeyMapping::applyCmdCtrlSwap(makeKeysym(SDL_SCANCODE_C, SDLK_c, KMOD_LGUI), true);
        QCOMPARE((int)c.scancode, (int)SDL_SCANCODE_C);
        QVERIFY(c.mod & KMOD_CTRL);
        QVERIFY(!(c.mod & KMOD_GUI));

        // Right Command + Shift + Z (redo) keeps Shift and becomes right Control
        SDL_Keysym z = KeyMapping::applyCmdCtrlSwap(makeKeysym(SDL_SCANCODE_Z, SDLK_z, KMOD_RGUI | KMOD_LSHIFT), true);
        QCOMPARE(z.mod, (Uint16)(KMOD_RCTRL | KMOD_LSHIFT));

        // Physical Control + C becomes GUI + C
        SDL_Keysym ctrlC = KeyMapping::applyCmdCtrlSwap(makeKeysym(SDL_SCANCODE_C, SDLK_c, KMOD_LCTRL), true);
        QCOMPARE(ctrlC.mod, (Uint16)KMOD_LGUI);
    }
};

int runKeyMappingTests(int argc, char** argv)
{
    TestKeyMapping tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_keymapping.moc"
