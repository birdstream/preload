# Möjliga optimeringar i preload-koden

Det finns några delar i den befintliga koden som kan trimmas för att minska
överhead och spara resurser utan att ändra programmets beteende.

## Åtgärdade problem
- `set_block()` stängde inte filbeskrivaren om `fstat()` misslyckades, vilket
  riskerade filhandtagsläckor under block-/inode-uppslag. Läckan är åtgärdad så
  att filen alltid stängs innan retur.
- `wait_for_children()` har ersatts av en icke-blockerande variant som bara
  väntar in en ledig plats när `maxprocs` är uppnått. Det gör att
  parallelliteten hålls uppe i readahead-slingan.
- Block-/inode-sorteringen återanvänder nu stat-info per fil, hoppar över dyra
  `open()`/`FIBMAP`-anrop för mycket små segment och behåller FIBMAP-resultatet
  om det faktiskt hämtas. Det minskar antalet syscalls när samma fil förekommer
  flera gånger.
- `preload_readahead()` slår ihop närliggande intervall över mindre gap (upp
  till ett block eller en sida), vilket minskar antalet `open()`- och
  `readahead()`-anrop när flera små segment av samma fil värms.
- Gaptröskeln följer filsystemets blockstorlek när den är känd och faller
  tillbaka på sidstorleken. Det gör sammanslagningen bättre anpassad till hur
  segmenten faktiskt hamnar i sidcachen.

## Kvarstående förbättringsmöjligheter
- Ytterligare hopslagning över filgränser (t.ex. genom att gruppera efter
  blocknummer) skulle kunna minska syscalls i fragmenterade miljöer. Det kräver
  dock större omstrukturering eftersom sorteringsstrategin blandar olika filer.
