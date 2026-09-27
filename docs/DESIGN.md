# Diseño provisional del juego

> **No existe `game.md`.** Por decisión del usuario (2026-09-26), el diseño lo decido yo a partir del prompt
> maestro y de las 4 capturas de referencia, y lo documento aquí para revisarlo. Todo lo de este documento es
> **provisional**: cuando exista `game.md`, prevalece. Los valores numéricos están en tablas de contenido
> (`Game/Sandbox/Content.h`) o en parámetros de generación, no dispersos por los sistemas, para que moverlos
> a ficheros de datos (modding) sea un cambio local.

## Estado: slice M3.4 "Los comerciantes también aceptan contratos"

Se puede:

- empezar una partida en un sistema generado y pilotar la nave (autopiloto con salto al hiperespacio, o
  empuje manual);
- jugar en tiempo real con aceleración ×3 o ×10, guardar y cargar, e inspeccionar cualquier entidad;
- detectar con **sensores y niebla de guerra** (M2.2);
- **combatir** (M2.3): armas que disparan a pistas de sensores, daño por módulos, piratas que cazan
  cargueros, reparaciones y reapariciones;
- **comerciar** (M3): cada puerto tiene un mercado con producción, consumo y precios que siguen a las
  existencias; los cargueros compran y venden por beneficio, y el jugador también;
- **cargar con las consecuencias** (M3.1): impuestos y reparaciones de pago a la Autoridad del sistema,
  salarios y quiebras de los comerciantes, recompensas por piratas, reputación según lo que los
  comerciantes han visto, y abordaje de naves sin energía;
- ver cómo la Autoridad **patrulla** con lo que recauda (M3.2): acude a las llamadas de socorro, persigue
  piratas y va a por ti si eres hostil;
- aceptar **contratos** (M3.3) que nacen de la situación real: suministros a puertos con escasez y
  recompensas por piratas identificados. Los cargueros compiten por los suministros (M3.4).

Con esto el primer vertical slice del prompt (§5) está completo.

**Todavía no hay:** crédito, deuda, seguros ni inversión; misiles ni defensa puntual; facciones políticas;
más de un sistema estelar.

## Escala, unidades y tiempo

| Decisión | Valor | Motivo |
|---|---|---|
| Unidades | metros, segundos, kg | físicas; la presentación convierte a km, km/s y UA |
| Marco de referencia | inercial centrado en la estrella; plano de la eclíptica = mapa | zoom continuo como en las referencias |
| Año de época | 2400 (`kEpochYear`) | futuro lejano; original |
| Calendario | 12 meses gregorianos, años de 365 días | fechas legibles ("2400-03-01"), determinismo trivial |
| Velocidades del tiempo | pausa, **×1 = tiempo real**, ×3, ×10 | directiva del usuario (ADR-021): se juega en tiempo real, como un simulador espacial; los viajes duran minutos |
| Consecuencia | el calendario avanza a ritmo real (×10 como máximo) | la escala de la economía, la política y las generaciones se diseñará para esta velocidad en M3–M5 |

## Sistema estelar (generación procedural)

- **Semilla** → estrella de 0,6 a 1,4 masas solares; luminosidad ∝ masa^3,5.
- **Planetas**: de 4 a 9; el primero entre 0,3 y 0,5 UA·√L y cada siguiente ×1,45–2,0 más lejos
  (parecido a los sistemas reales, sin copiar ninguno).
- **Tipo según la distancia**: rocosos y desérticos dentro; oceánicos en la zona habitable (0,95–1,4 UA·√L);
  gigantes gaseosos y helados más allá de la línea de hielo (2,7 UA·√L). Radios y densidades según el tipo.
- **Lunas**: 1–4 en gigantes, 0–2 en helados, 0–1 en rocosos, siempre dentro del 30 % de la esfera de Hill.
- **Estaciones**: el puerto principal orbita el planeta más habitable y la segunda estación un gigante
  gaseoso (o el último planeta). Así hay dos polos de tráfico.
- **Nombres**: inventados con sílabas propias (estrella), numeración romana (planetas), letras (lunas) y
  títulos en español para las estaciones ("Puerto", "Estación", "Atalaya", "Muelle", "Enclave", "Relé").
  Nunca se usan catálogos reales ni nombres de otros juegos.
- **Órbitas**: raíles keplerianos analíticos. Los cuerpos no se integran y no derivan.

## Modelo de vuelo (ADR-022)

- **Dos motores**, como en las referencias: **sublumínico** para maniobrar y atracar (newtoniano), e
  **hiperespacio** para cruzar el sistema (millones de km/s, trayecto recto).
- **Pozos gravitatorios**: estrella = 20 radios y planetas = 25 radios (círculos discontinuos en el mapa).
  Dentro no se puede saltar, y el salto termina en el borde del pozo del destino con la velocidad igualada
  a la suya.
- **El autopiloto decide solo**: si el tramo en hiperespacio supera 1 millón de km y la nave está fuera de
  un pozo, carga el salto (5–12 s, sin dejar de avanzar), salta, sale junto al destino y termina en
  sublumínico. Cualquier otra orden cancela la carga o provoca una salida de emergencia.
- **Newtoniano con impulsor de ciencia ficción** (sublumínico): la nave tiene inercia y un empuje máximo
  alto; si no hay empuje, sigue derivando. **Sin gravedad sobre las naves** en este slice (sí sobre los cuerpos, vía
  órbitas): con aceleraciones de cientos de g la gravedad planetaria es despreciable en viaje, y aplicarla
  complicaría el pilotaje sin aportar juego todavía. Se reconsiderará con combate orbital.
- **Velocidad de crucero**: límite del autopiloto, no físico. Con empuje manual se puede superar.
- **Autopiloto**: modos Deriva, Detenerse, Ir a punto, Aproximación (a un cuerpo o una nave, manteniendo una
  distancia de seguridad) y Manual. El perfil acelera, navega a crucero y frena al 80 % del empuje máximo, y
  nunca se pasa del objetivo.
- **Distancias de seguridad**: estaciones a 5 km, naves a 2 km, planetas y lunas a 1,25 radios + 200 km, y la
  estrella a 3 radios.
- **Mantener posición** junto a un cuerpo que orbita exige empujar de forma continua (se ve la llama): es
  el precio de no tener gravedad sobre las naves.
- **LOD del vuelo**: pasos de 200 ms (estratégico) mientras nadie pilota a mano, y de 50 ms (táctico)
  mientras el jugador usa el empuje manual. El cambio se hace dentro de la simulación, por comando, y es
  determinista.

### Clases de nave

| Clase | Rol | Sublumínico: empuje / crucero | Hiperespacio | Carga del salto | 1 UA en hiperespacio |
|---|---|---|---|---|---|
| Correo | nave del jugador | 50 km/s² / 15.000 km/s | 1.500.000 km/s (5 c) | 5 s | ~100 s |
| Carguero | transporte NPC | 15 km/s² / 6.000 km/s | 600.000 km/s (2 c) | 12 s | ~250 s |
| Corsario | pirata NPC | 40 km/s² / 12.000 km/s | 1.200.000 km/s (4 c) | 6 s | ~125 s |

Las referencias muestran velocidades sublumínicas de ~20.000 km/s y velocidades de hiperespacio de más de
un millón de km/s. Con ×1 = tiempo real, un viaje entre planetas dura de 3 a 6 minutos a ×1 (menos de un
minuto a ×10).

## Sensores (ADR-023)

- **Nadie es omnisciente**: solo ves tu flota y lo que detectan tus sensores. Los cuerpos celestes sí se
  conocen (cartas del sistema).
- **Pasivo** (siempre activo): detecta según la emisión del otro. Una nave quieta es casi invisible (a unos
  22.000 km para los sensores de un carguero), una que acelera fuerte se ve a unos 700.000 km, y cualquier
  salto al hiperespacio se ve desde casi todo el sistema.
- **Radar (R)**: identifica un carguero a unos 3,5 millones de km con buena precisión, pero su emisión te
  hace visible a unos 7 millones de km.
- **Transpondedor (T)**: difunde tu identidad y posición hasta 1 UA. Los cargueros lo llevan encendido. Si
  lo apagas, cuesta mucho más identificarte.
- **Identificar sin transpondedor** exige una señal clara (SNR pasivo ≥ 400, o SNR de radar ≥ 4). Un
  Correo quieto es identificado por un carguero a unos 1.100 km, clasificado a unos 11.000 km y detectado a
  unos 44.700 km (ADR-029).
- **Contactos**: desconocido (?), clasificado ("Carguero?") o identificado (nombre), con círculo de
  incertidumbre. Si dejan de detectarse se atenúan y se pierden a los 20 s. El ruido crea a veces contactos
  fantasma que desaparecen.
- **Ir a un contacto** lleva a su posición estimada, no a la real.

| Clase | Emisión base | Emisión a pleno empuje | Sección radar | Sensibilidad pasiva | Radar |
|---|---|---|---|---|---|
| Correo | 1e3 | +1e6 | 1e3 m² | 1e12 | sí |
| Carguero | 5e3 | +2e6 | 1e4 m² | 5e11 | no |
| Corsario | 2e3 | +1,5e6 | 3e3 m² | 1e12 | sí |

## Combate (ADR-024, ADR-025)

**Módulos.** Una nave no es una barra de vida, sino un conjunto de módulos con su propia salud (como en el
editor de naves de las referencias). La estructura va primero: si llega a cero, la nave se destruye.

| Diseño | Estructura | Reactor | Motor | Hipermotor | Sensores | Armas | Otros |
|---|---|---|---|---|---|---|---|
| Correo | 500 | 150 | 150 | 120 | 100 | láser 80, riel 80 | habitáculo 80 |
| Carguero | 600 | 150 | 200 | 150 | 80 | — | bodega 600, habitáculo 150 |
| Corsario | 400 | 120 | 150 | 120 | 80 | 2 láseres 80, riel 80 | habitáculo 80 |

**Daño.**

- Cada impacto hace dos cosas: la estructura absorbe la mitad, y un módulo elegido al azar en proporción
  a su tamaño recibe el impacto completo. Si ese módulo ya está destrozado, su parte pasa a la estructura.
- Por debajo del 10 % un módulo queda fuera de servicio.
- Sin reactor, la nave queda **sin energía**: no tiene motor, ni hipermotor, ni armas, ni radar, y va a la
  deriva.
- Cuando el reactor queda fuera de servicio, hay un 30 % de probabilidad de brecha, que destruye la nave.
- Motor y sensores escalan con su salud. El hipermotor no funciona por debajo del 50 %, y un salto en curso
  se aborta si se pierde.

**Armas.**

| Arma | Tipo | Alcance | Daño | Notas |
|---|---|---|---|---|
| Láser | haz (instantáneo) | 800 km | 4 por segundo en el blanco | acierta si el error de puntería es menor que el casco |
| Cañón de riel | proyectil a 5.000 km/s | 3.000 km | 15 por impacto | recarga de 2 s; el proyectil vuela en línea recta |

**Control de tiro.**

- Se dispara a **pistas de sensores**, no a naves.
- Para disparar hace falta que tu facción tenga el contacto y que tus propios sensores lo fijen (SNR ≥ 1).
  A un fantasma no se le puede disparar.
- El error de puntería suma tres cosas: el error de medida (d/√SNR, pequeño con radar), el error del arma,
  y lo que el blanco acelera durante la latencia del control de tiro (50 ms).
- Un Correo que acelera a fondo esquiva parte de los láseres. Los proyectiles se apuntan suponiendo
  velocidad constante, así que a largo alcance una nave que acelera los esquiva.
- El daño está ajustado para que un duelo dure alrededor de un minuto a ×1. Da tiempo para huir, maniobrar
  o apagar el radar. Medido: un corsario destruye al Correo, que no responde, en 52 s.

**Reparaciones y pérdidas.**

- Atracado en una **estación**, todo se repara al 2 % por segundo.
- Fuera de puerto, el control de daños lleva el reactor y el motor hasta el 25 %, a 0,25 % por segundo,
  tras 10 s sin recibir impactos. Un reactor destrozado vuelve a dar energía en unos 50 s.
- Si destruyen tu nave, a los 10 s te espera una nueva en la estación principal.
- Los cargueros perdidos se reponen al ritmo de uno por minuto, y los piratas de uno cada 3 minutos (el
  sistema no es una caja cerrada).

## Economía (ADR-027, ADR-028)

**Cadena de suministro.** En cada sistema, los planetas extraen según su tipo; las estaciones fabrican:

| Puerto | Produce | Con | Consume (población) |
|---|---|---|---|
| Planeta helado | agua | mantenimiento: maquinaria | alimentos, combustible |
| Planeta oceánico | alimentos | mantenimiento: maquinaria | combustible |
| Planeta rocoso / desértico | mineral | mantenimiento: maquinaria | alimentos, agua |
| Gigante gaseoso | combustible | mantenimiento: maquinaria | alimentos, agua |
| Estación refinería | metales | 2 t de mineral + 0,5 t de combustible por t | alimentos, agua |
| Estación fábrica | maquinaria | 2 t de metales + 0,5 t de combustible por t | alimentos, agua |

- Sin insumos esenciales no hay producción.
- Sin mantenimiento (maquinaria), la extracción cae al 35 %: la cadena se cierra sobre sí misma.
- Al generar el sistema, cada bien se produce en total un 20 % por encima de lo que se consume. Si un
  bien no lo puede extraer ningún planeta, la primera estación lo fabrica (hidroponía, recicladoras). Así
  la escasez local depende del transporte y de los piratas, no de la suerte del generador.

| Bien | Precio base (cr/t) |
|---|---|
| Agua | 20 |
| Alimentos | 40 |
| Mineral | 30 |
| Combustible | 50 |
| Metales | 120 |
| Maquinaria | 300 |

**Precios.**

- Cada puerto guarda un **objetivo de existencias**: 4 horas de su producción o de su consumo, el mayor
  de los dos. Su capacidad es el triple.
- Precio medio = base · 2^(1,5 · (1 − existencias/objetivo)): la base en el objetivo, ×2,83 con el almacén
  vacío y ×0,21 con 2,5 veces el objetivo.
- El puerto vende un 4 % por encima y compra un 4 % por debajo.
- Las operaciones se cobran tonelada a tonelada, así que un pedido grande mueve el precio en su contra.

**Conocimiento.** Nadie lee un mercado remoto:

- Cada facción tiene un **libro de precios** con la edad de cada observación. Al empezar, todos tienen el
  boletín inicial del sistema.
- Los comerciantes independientes comparten una red: cada carguero que atraca actualiza los precios de
  todos.
- El jugador actualiza los precios del puerto donde atraca, y las **estaciones le dan el boletín** de los
  comerciantes.

**Cargueros comerciantes.** Al salir de puerto, cada carguero:

1. Si lleva carga, va donde mejor se vende.
2. Si no, compra aquí lo que más rinde en otro puerto: beneficio por segundo de viaje, descontando precios
   viejos (a la mitad cada 30 min) y el peligro del destino.
3. Si aquí no hay nada rentable, va vacío a donde sabe que hay una ganga (reposicionamiento).
4. Si tampoco, va a refrescar los precios más viejos.

Al estimar lo que obtendrá, sigue la curva de precios real y cuenta con las entregas que la red ya tiene
en camino a ese puerto. Así varios cargueros no hunden a la vez el mismo mercado.

El **peligro** de un puerto sube cada vez que muere un carguero que iba hacia él y se olvida a la mitad
cada 20 minutos.

**Causalidad medida** (10 h, 12 cargueros, con y sin 3 piratas):

| Semilla | Entregas | Escasez de agua | Escasez de alimentos |
|---|---|---|---|
| 2400 | 16.949 → 11.680 t (−31 %) | 256 → 472 t | 238 → 634 t |
| 7 | 14.977 → 11.476 t (−23 %) | 132 → 712 t | 58 → 167 t |

**Tú.**

- Empiezas con 2.000 cr y 20 t de bodega (los cargueros llevan 100 t).
- Atracado en un puerto, compras y vendes en la ventana **Mercado**.
- Los precios verdes son baratos y los rojos caros, respecto al precio base.
- Seleccionando un puerto ves los precios que conoces y su antigüedad.
- Si te destruyen, pierdes la carga pero no los créditos.

## Consecuencias (ADR-029)

**La Autoridad del sistema** (las estaciones y los puertos) tiene tesorería propia. Empieza con 20.000 cr.

- Cobra un **3 % de impuesto** sobre cada tonelada comprada o vendida. Lo paga el comerciante, encima del
  precio al comprar y descontado al vender.
- Cobra las **reparaciones en estación**: 3 cr por punto de salud reparado. Sin dinero no hay reparación.
- Paga **recompensas** de 1.500 cr por cada pirata destruido o capturado por el jugador, mientras le
  queden fondos.

**Tripulaciones.**

- Cada carguero paga 5 cr por minuto de salarios (300 cr/h), en puerto o en ruta.
- Un comerciante en números rojos **quiebra** al intentar salir de puerto: vende lo que tiene, abandona el
  sistema y otro lo sustituye más tarde (con capital nuevo).

**Reputación con la Autoridad y los comerciantes** (−100 a 100; empieza en 0):

| Acción | Cambio |
|---|---|
| Atacar un carguero, si su red te identifica (un golpe por nave cada 60 s) | −10 |
| Destruir un carguero, si te identifican | −30 |
| Abordar un carguero (siempre saben quién fue) | −20 |
| Destruir o capturar un pirata | +5 |
| Con el tiempo, si es negativa | +0,5 por minuto hacia 0 |

- **Testigos, no omnisciencia:** la reputación solo cae si la red de comerciantes te tiene *identificado*
  en sus sensores en ese momento.
- Con el transpondedor apagado, quieto y disparando con el cañón de riel desde 1.100–3.000 km, puedes
  atacar sin que sepan quién eres. A cambio, sin radar la puntería es mala: el radar te delataría.
- Con reputación **hostil** (−30 o menos), los puertos no comercian contigo, las estaciones no te reparan
  ni te dan su boletín de precios.

**Abordaje (B).** A una nave **sin energía** (reactor fuera de servicio), a menos de 5 km y con la
velocidad igualada (±200 m/s):

- te llevas la carga que quepa en tu bodega, y el resto se pierde;
- la tripulación abandona la nave, que desaparece;
- capturar así a un pirata también paga recompensa.

**Medido** (10 h, semilla 2400, 3 piratas):

- la tesorería pasa de 20.000 a ~94.000 cr (44.500 de impuestos y 29.500 de reparaciones);
- los salarios retiran 35.900 cr;
- hay 3 quiebras.

La tesorería paga las patrullas (ver abajo).

## Patrullas de la Autoridad (ADR-030)

**Presupuesto, con reglas explícitas.** Cada 2 minutos la Autoridad:

- paga el mantenimiento de sus patrulleros (25 cr/min cada uno, 1.500 cr/h);
- pone en servicio uno nuevo si tiene su precio (6.000 cr) más 4 horas de mantenimiento de la flota
  ampliada, hasta un máximo de 3;
- da de baja uno si la tesorería queda en negativo.

Todo sale en el diario como noticia.

**Patrullero** (Autoridad): 45 km/s², crucero de 14.000 km/s, hiperespacio a 1.400.000 km/s. Estructura
600, dos láseres y un cañón de riel. Tiene los mejores sensores del sistema (sensibilidad 2e12, radar 5e34)
y va a la vista: radar y transpondedor siempre encendidos.

**IA del patrullero.** Decide solo con la imagen de sensores de la Autoridad:

1. **Sospechosos:** piratas identificados, cualquier contacto clasificado como Corsario, y tú si eres
   hostil. Persigue al más cercano a menos de 1 millón de km (a 150 km) y dispara. Abandona si lo pierde,
   si se aleja más de 2 millones de km o a los 5 minutos.
2. **Socorro:** un carguero atacado avisa con su posición por la red de comerciantes. Acude el patrullero
   libre más cercano.
3. **Ronda:** si no hay nada que hacer, vigila el borde del pozo del planeta con más pérdidas recientes.
4. **Reparación:** con la estructura por debajo del 50 % o sin armas, vuelve a la estación más cercana
   (reparación a cargo de la Autoridad).

**Los piratas** no eligen patrulleros como presa y rompen la caza si hay uno a menos de 300.000 km.

**Tú y la Autoridad:**

- Atacar un patrullero cuesta −15 de reputación (−40 si lo destruyes), si la Autoridad te identifica.
- Hostil, te persiguen.
- Si te destruyen siendo hostil, tu cuenta queda saldada (reputación −20) y no te esperan a la salida de la
  estación.

**Medido** (10 h, 3 piratas, 5 variantes de la semilla 2400):

| | Cargueros perdidos (media) | Entregas |
|---|---|---|
| Sin patrullas | 18,2 | 11.220–13.301 t |
| Con 3 patrullas | 10,0 (−45 %) | 9.793–14.449 t |

- Las entregas no cambian más allá del ruido. Cada carguero perdido vuelve con capital nuevo, lo que
  enmascara el daño.
- Sin las llamadas de socorro, las patrullas no servían: llegaban tarde a donde ya no había piratas
  (ADR-030).

## Contratos (ADR-031)

Todo contrato sale del estado real de la simulación; no hay misiones inventadas.

| Tipo | Cuándo se publica | Qué pide | Quién paga | Plazo |
|---|---|---|---|---|
| Suministro | un puerto tiene menos del 25 % de su objetivo de un bien que consume | 20 t de ese bien entregadas allí | el propio puerto: 20 t × precio base × 2 (800 cr de agua, 1.600 de alimentos...) | 45 min |
| Recompensa | la Autoridad o la red de comerciantes tienen identificado a un pirata | abatirlo o capturarlo | la tesorería: 2.500 cr, además de la recompensa normal | 60 min |

- **Tablón:** hay como máximo 6 contratos publicados y un suministro por puerto a la vez. Se ven y se
  aceptan atracado en una **estación**, hasta 3 a la vez, y no se dan a quien es hostil. La ventana
  Contratos (K) muestra los tuyos en todo momento, con lo entregado y el tiempo restante.
- **Entrega:** atracado en el puerto de destino. La mercancía entra en sus existencias (alivia la escasez
  de verdad), puede ser parcial, y al completarla cobras y ganas +3 de reputación.
- **Fallar o abandonar:** −5 de reputación. Si el pirata muere a manos de otro o se va, el contrato se
  cancela sin penalización.
- **La Autoridad prioriza sus patrullas.** Solo reserva una recompensa (la retira de la tesorería hasta
  que se cobra o caduca) si le sobra dinero después de la próxima compra de patrullero, 4 h de
  mantenimiento de la flota y 5.000 cr de margen.

**Los comerciantes compiten por los suministros (ADR-032).**

- La red de comerciantes comparte el tablón, igual que el boletín de precios.
- Al planificar un viaje, un carguero suma la recompensa de un suministro abierto en ese destino y para ese
  bien, si su carga lo cubre entero.
- Si su mejor plan lo usa, lo acepta y el contrato desaparece del tablón. Al llegar entrega esa parte en
  las existencias del puerto y vende el resto.
- Si el carguero muere o quiebra antes, el contrato vuelve al tablón.
- Nunca toman los contratos que has aceptado tú.

Medido en 8 h y 4 semillas:

- la escasez total baja un 30 % (de 2.942 a 2.046 t);
- las entregas suben un 13 % (de 31.084 a 35.241 t);
- en las cuatro semillas en la misma dirección.

**Medido** (8 h, 4 semillas, 3 piratas, hasta 3 patrullas): se publican entre 18 y 43 contratos. Con
patrullas se pierden 31 cargueros frente a 46 sin ellas (−33 %). Cuando las recompensas salían de la
misma tesorería sin prioridad, había semillas con solo 2 patrullas y el efecto bajaba al −26 %.

## Facciones y personajes (mínimo)

- **Jugador** (verde), **Transportistas independientes** (azul) y **Piratas** (rojo). La política, las
  Casas y los títulos llegarán en la fase 5.
- Nave del jugador: "Errante" (Correo). Cargueros: nombre + número ("Faro-12", "Nómada-47"). Corsarios:
  "Colmillo-66", "Sombra-86"...

## Comportamiento de los NPC

- Regla simple y reproducible, sin LLM (prompt §16): esperar en puerto de 30 s a 3 min, elegir **otro** puerto
  al azar (flujo aleatorio por nave y viaje), volar (con salto) y atracar, y repetir.
- La galaxia actúa sin el jugador: los cargueros viajan y atracan aunque el jugador no haga nada, y cada
  llegada queda en el diario.
- Un carguero atacado huye a la estación más cercana, donde los piratas no se atreven a entrar.
- Desde M3 los cargueros comercian: eligen carga y destino por beneficio con lo que sabe su red (ver
  Economía).

**Piratas (ADR-026).** Deciden solo con la imagen de sensores de su facción, nunca con la verdad:

- **Acecho:** esperan justo fuera del pozo de un planeta, que es donde los cargueros salen del
  hiperespacio. Van en silencio (sin transpondedor ni radar) y cada 4 minutos cambian a uno de los 3
  planetas más cercanos.
- **Caza:** eligen la pista clasificada más cercana, siempre que no sea un corsario y esté a menos de
  300.000 km. Descartan las pistas en hiperespacio y las que están a menos de 50.000 km de una estación.
  Encienden el radar, persiguen la pista a 200 km y disparan.
- **Abandono de la caza:** abandonan si pierden la pista, si la presa se aleja más de 600.000 km o llega a
  una estación, o a los 3 minutos (y entonces descartan esa pista).
- **Huida:** con la estructura por debajo del 40 % o sin armas, salen del sistema por las afueras y
  desaparecen de la simulación.
- El jugador con el transpondedor encendido es tan presa como un carguero. Apagarlo y no acelerar cerca de
  los pozos es la forma de pasar desapercibido.
- **Medido** (3 h, 12 cargueros, 3 piratas, 4 semillas): de 30 a 61 cacerías y de 3 a 7 cargueros
  perdidos. Es decir, uno o dos por hora.

## Controles del cliente

| Acción | Control |
|---|---|
| Zoom (de metros a UA) | rueda del ratón |
| Mover la vista | arrastrar con el botón izquierdo |
| Seleccionar | clic |
| Ir a un objeto o a un punto | clic derecho |
| Empuje manual | W A S D (arriba = +y del mapa) |
| Frenar | X |
| Radar / transpondedor | R / T |
| Atacar el contacto seleccionado (lo persigue a 300 km y dispara) | E |
| Alto el fuego | C |
| Interceptar sin disparar | botón en la selección del contacto |
| Comprar / vender | ventana Mercado, atracado en un puerto (+1, +10, −1, Todo) |
| Abordar el contacto seleccionado (sin energía, a < 5 km, velocidad igualada) | B |
| Contratos | K (se aceptan atracado en una estación) |
| Pausa / velocidad | Espacio / 1 (×1), 2 (×3), 3 (×10) |
| Seguir tu nave / la selección | H / F |
| Guardar / cargar | F5 / F9 |
| Depuración / ayuda | F3 / F1 |

## Lenguaje visual

- Mapa 2D oscuro con órbitas tenues, colores por tipo de cuerpo, triángulos orientados por la velocidad
  para las naves, llama de empuje, línea de rumbo, barra de escala en km o UA, y etiquetas según el zoom.
- Diario: lo tuyo en blanco y las noticias en ámbar. El tráfico de otras naves está oculto salvo que marques
  la casilla.
- Paneles: tiempo (arriba), nave (izquierda, con barras de salud por módulo y estado de cada arma: fijado,
  sin fijación, fuera de alcance o recargando), selección con inspector (derecha), diario (abajo a la
  izquierda) y depuración (F3).
- Combate: los haces son cian (los tuyos) o naranjas (hostiles), y tenues cuando fallan. Los proyectiles
  son puntos con estela, y las explosiones, anillos que se expanden. El blanco de tus armas lleva una
  retícula roja. Solo ves el fuego a menos de 10.000 km de tu nave y las explosiones a menos de 5 millones
  de km, salvo en la vista de depuración.
- Referencias: solo conceptuales. La interfaz, los nombres y los assets son originales.

## Pendiente de decidir (para `game.md` o para próximos slices)

1. Guerra electrónica (interferencias, señuelos) y consecuencias legales directas de ir sin transpondedor.
2. Misiles, defensa puntual, blindaje o escudos.
3. Piratas que saquean en lugar de destruir; sensores fijos en las estaciones; patrullas que escoltan
   convoyes.
4. Resto de la economía del §18: crédito, deuda, seguros e inversión (comerciantes que compran naves).
5. Población con efectos: la escasez debería afectar a la estabilidad y al crecimiento.
6. Viaje entre sistemas y la estructura de la galaxia.
7. Nombre del juego, tono y estética definitiva.
