# android-share-contact (TODO)

**Status: planned.** Waits for the Android session layer (Kotlin port of docs/SESSION.md).

Planned example: a Compose app that sends a contact as a compact vCard (`BEGIN:VCARD` / `FN` / `TEL` / `EMAIL` / `END:VCARD`, about 100 characters, a few seconds on `mid`) and receives one: listen, show the parsed fields, pass the confirm gate, then offer *Add to contacts* via `ContactsContract` intent (nothing is saved without a tap). A vCard is not secret: the example says so and lets the sender strip fields. Contact picker on the sender side.
