# android-share-contact

A minimal Android app (one Activity, plain Views): send a contact as a compact vCard 3.0 by sound, receive one, and add it with a tap.

- **Send:** name (required), phone and email (optional: leave them empty to strip them). The text is `BEGIN:VCARD / VERSION:3.0 / FN / TEL / EMAIL / END:VCARD`, about 70 to 110 characters, sent as a session with the channel chosen automatically (channel ladder); it shows `Delivered on <channel>`.
- **Receive:** *Listen*, then `result` and `confirmed()`. The text is parsed strictly (`VCard.kt`: one card, a name, plausible phone, short fields); anything else is ignored. The parsed contact is shown with *Add to contacts*, which fires `ContactsContract.Intents.Insert.ACTION`: the Contacts app opens prefilled and nothing is saved until the person taps Save there. No contacts permission is needed. Only `RECORD_AUDIO` (from the library manifest) is requested.

It is its own Gradle project; `settings.gradle` includes the library from `../../android/bysound`, and the wrapper is copied from `android/`.

## Run

```sh
cd examples/android-share-contact
echo "sdk.dir=$ANDROID_HOME" > local.properties        # once, unless ANDROID_HOME is set
./gradlew assembleDebug && adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Use two phones, a loud speaker and no headphones or Bluetooth.

## Security note

Sound is not secret and not authenticated: anyone in earshot hears the contact, and anyone can send you one that claims to be somebody else (a fake "Bank" with a scam number is the classic). The confirm gate only says that no second emitter or risk frame was heard. So the receiver validates and shows the content, never saves it by itself, and leaves the final decision to the Contacts screen. Do not send data you would not say aloud.
