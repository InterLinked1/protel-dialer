# protel-dialer

**Outbound Protel dialer daemon for use with Asterisk softmodem**

This is a simple daemon program that can be used for saving transcripts of outbound modem sessions using the Asterisk softmodem,
specifically geared towards saving transcripts of calls to modems such as those used in Protel COCOTs.

This program does not dial numbers or do anything telephony related; that's where integration with the aforementioned softmodem comes in.

### Compiling and Running

Clone the repository and just run `make`. Seriously, that's it. Then you can run `./proteld` with the desired options. Run `./proteld -h` for usage.
