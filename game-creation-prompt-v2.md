# PROMPT MAESTRO — CREACIÓN DEL JUEGO Y MOTOR DE SIMULACIÓN GALÁCTICA

## Objetivo

Desarrolla un videojuego de estrategia 4X, sandbox y simulación galáctica de enorme escala. No debe ser solamente un 4X tradicional: el objetivo es construir un **universo persistente y vivo** donde economía, política, personajes, organizaciones, colonias, comercio, logística, diplomacia, guerras, flotas y exploración continúen funcionando aunque el jugador no esté presente.

La fantasía central es:

> **La galaxia existe, cambia y produce consecuencias aunque el jugador no la esté mirando.**

El jugador puede empezar como una persona con una nave pequeña y terminar dirigiendo una Gran Casa, una corporación, un territorio, una federación o una potencia interestelar.

---

# 1. DOCUMENTOS DE AUTORIDAD

Antes de escribir código debes leer:

- `game.md`
- `CLAUDE.md`
- todos los archivos de `referencias/`
- cualquier documentación técnica existente

`game.md` define la visión y los sistemas de juego. `CLAUDE.md` define las reglas operativas del repositorio. Las imágenes de `referencias/` son referencias visuales de UX, composición, densidad de información, mapas y navegación.

No copies nombres, textos, iconos, assets, interfaces ni diseños propietarios. Usa las imágenes solamente como referencia conceptual para crear una interfaz original.

Si existe una contradicción entre documentos, identifícala y resuélvela antes de construir una arquitectura incompatible.

---

# 2. CAMBIO FUNDAMENTAL DE ARQUITECTURA

El objetivo técnico ya no es construir este proyecto encima de un motor comercial como dependencia central. El proyecto debe diseñarse como un **motor propio en C++ orientado específicamente a simulación masiva, física espacial, destrucción, procesamiento paralelo y escalabilidad**.

Tecnología objetivo:

- C++ moderno;
- build reproducible;
- arquitectura modular;
- Job System / Task System;
- simulación determinista cuando sea posible;
- simulación headless;
- renderizado separado del estado de simulación;
- serialización/versionado de partidas;
- profiling y benchmarks desde el inicio;
- herramientas internas de depuración;
- soporte para multithreading seguro.

Las librerías externas están permitidas cuando aporten un beneficio claro, pero la simulación central no debe quedar subordinada a un motor comercial.

**Nota importante:** el `game.md` actual contiene una propuesta de Godot 4 en su sección de arquitectura técnica. Esa sección debe tratarse como una propuesta anterior que queda reemplazada por esta decisión de motor propio en C++.

---

# 3. PRINCIPIOS DE DISEÑO DEL MOTOR

Prioridades, en este orden:

1. Correctitud de la simulación.
2. Escalabilidad.
3. Determinismo y reproducibilidad.
4. Rendimiento medible.
5. Capacidad de depuración.
6. Moddabilidad.
7. Presentación.

No optimices a ciegas. **Mide primero.**

No construyas una arquitectura con miles de entidades complejas permanentes si el mismo comportamiento puede representarse de forma agregada.

---

# 4. REGLA PRINCIPAL: NO CONSTRUIR TODO DE GOLPE

Claude Code NO debe intentar crear toda la galaxia, economía, política, combate y destrucción en una sola fase.

El desarrollo debe avanzar por **vertical slices ejecutables**.

Orden inicial:

```text
Engine Core
↓
Simulation Kernel
↓
Time System
↓
Job System
↓
Headless Simulation
↓
Small Space Sandbox
↓
Ships + Movement
↓
Sensors
↓
Combat
↓
Damage
↓
Economy
↓
AI
↓
Politics
↓
Colonies
↓
Massive Simulation
↓
Advanced Destruction
↓
Content + UX
```

Cada fase debe compilar, ejecutarse, poder probarse y tener métricas.

---

# 5. PRIMER VERTICAL SLICE

Construye primero un sandbox pequeño con:

- 1 sistema estelar;
- 1 estrella;
- planetas/cuerpos básicos;
- 1 estación;
- varias naves;
- 1 jugador;
- NPCs;
- movimiento espacial;
- sensores activos y pasivos básicos;
- combate básico;
- daño por subsistemas;
- economía mínima;
- comercio;
- tiempo acelerable;
- save/load;
- debug inspector;
- profiling;
- tests.

Debe ser posible iniciar una partida, pilotar una nave, detectar otra, comerciar, combatir, recibir daños, perder sistemas, acelerar el tiempo, observar NPCs actuando, guardar/cargar e inspeccionar el mundo.

No ampliar el alcance hasta que este slice sea estable.

---

# 6. ARQUITECTURA DEL MOTOR

Estructura sugerida:

```text
Engine/
├── Core/
├── Memory/
├── Math/
├── Jobs/
├── Time/
├── ECS/
├── Serialization/
├── Events/
├── Debug/
├── Profiling/
├── Input/
├── Rendering/
└── Tools/

Simulation/
├── World/
├── Galaxy/
├── Entities/
├── Characters/
├── Organizations/
├── Economy/
├── Logistics/
├── Politics/
├── Diplomacy/
├── Military/
├── Colonies/
├── Missions/
├── AI/
└── History/

Space/
├── Ships/
├── Fleets/
├── Sensors/
├── Weapons/
├── Missiles/
├── Damage/
├── Physics/
└── Destruction/

Game/
├── Player/
├── Progression/
├── Contracts/
├── UI/
└── SaveGame/
```

La estructura puede cambiar si existe una razón técnica demostrable.

---

# 7. SIMULACIÓN DESACOPLADA DEL RENDER

La simulación debe ejecutarse sin renderizado.

Crear desde temprano un modo:

```text
HEADLESS SIMULATION
```

para:

- tests;
- benchmarks;
- balance;
- generación procedural;
- simulación de años;
- reproducción de bugs;
- pruebas de determinismo.

La UI no puede ser necesaria para que economía, IA, política o tiempo funcionen.

---

# 8. SIMULACIÓN MULTIESCALA Y LOD LÓGICO

No todas las entidades deben actualizarse a la misma frecuencia ni con el mismo detalle.

Definir como mínimo:

```text
LOD 0 — ABSTRACTO
Regiones y organizaciones lejanas

LOD 1 — REGIONAL
Sistemas alejados y flotas agregadas

LOD 2 — SYSTEM
Sistema relevante y entidades simplificadas

LOD 3 — LOCAL
Área del jugador y entidades individuales

LOD 4 — REALTIME
Combate, física y destrucción detallada
```

Una flota puede pasar de:

```text
flota agregada
→ subgrupos
→ naves individuales
→ componentes
→ física detallada
```

El cambio de LOD no debe destruir ni regenerar arbitrariamente el estado. Debe conservar continuidad.

---

# 9. JOB SYSTEM Y PARALELISMO

Crear una infraestructura de tareas paralelas para candidatos como:

- IA;
- economía;
- producción;
- población;
- logística;
- sensores;
- generación procedural;
- eventos;
- pathfinding.

No paralelizar por moda. Identifica dependencias y usa fases seguras.

Ejemplo:

```text
Input
↓
Commands
↓
Simulation Jobs
↓
Synchronization
↓
Event Resolution
↓
History
↓
Presentation Snapshot
↓
Rendering
```

---

# 10. ENTIDADES Y DATOS

Separar claramente:

- identidad;
- estado;
- comportamiento;
- representación visual;
- persistencia.

Evitar miles de objetos pesados con jerarquías innecesarias.

Usar estructuras eficientes en memoria y cache locality. ECS solo cuando sea útil; no introducirlo por moda.

---

# 11. FÍSICA ESPACIAL

El sistema debe manejar grandes distancias y velocidades relativas.

Debe considerar:

- posición;
- velocidad;
- aceleración;
- masa;
- orientación;
- trayectoria;
- gravedad cuando corresponda;
- colisiones;
- proyectiles;
- misiles;
- fragmentos.

Investiga una solución de precisión apropiada, incluyendo double precision, origin rebasing o coordenadas jerárquicas si resulta necesario.

---

# 12. DESTRUCCIÓN Y DEBRIS

Las naves y estaciones relevantes deben poder sufrir daños localizados.

No usar únicamente un HP global.

Ejemplo:

```text
Nave
├── Estructura
├── Reactor
├── Motores
├── Sensores
├── Comunicaciones
├── Armas
├── Munición
├── Depósitos
├── Habitáculos
└── Tripulación
```

Una secuencia de daño puede ser:

```text
Impacto
↓
Daño estructural
↓
Daño de componente
↓
Pérdida funcional
↓
Efectos secundarios
↓
Fragmentación / incendio / despresurización
```

La destrucción de alta resolución solo debe activarse cerca del foco relevante. El universo lejano debe usar representación agregada.

---

# 13. COMBATE

El combate debe depender de:

- distancia;
- velocidad relativa;
- aceleración;
- orientación;
- firma térmica/electromagnética;
- sensores;
- energía;
- munición;
- daño de subsistemas;
- logística.

Objetivos posibles:

- destruir;
- incapacitar;
- escapar;
- escoltar;
- capturar;
- bloquear;
- sobrevivir;
- proteger un objetivo.

---

# 14. SENSORES

### Pasivos

- menor firma propia;
- mayor discreción;
- información incompleta;
- dependencia de emisiones del objetivo.

### Activos

- mayor precisión;
- mejor seguimiento;
- mayor firma propia.

Incluir incertidumbre, ruido, falsos positivos, identificación parcial y pérdida de contacto.

La IA y el jugador NO deben tener omnisciencia.

---

# 15. MISILES Y ARMAS

Los misiles pueden tener:

- propulsión;
- combustible;
- sensores;
- guiado;
- ojiva;
- firma.

A escala macro se pueden agrupar; a escala táctica pueden materializarse individualmente.

Soportar armas cinéticas, armas de energía, interceptores, defensa puntual, contramedidas y guerra electrónica según las reglas del `game.md`.

---

# 16. IA

No utilizar un LLM como cerebro de cada NPC.

La simulación debe usar algoritmos reproducibles:

- Utility AI;
- GOAP;
- planificación;
- reglas;
- necesidades;
- objetivos;
- memoria de eventos;
- comportamiento basado en información incompleta.

Cada IA debe actuar según objetivos, recursos, riesgos y conocimiento disponible.

---

# 17. AI INSPECTOR

Crear una herramienta interna que permita inspeccionar:

```text
Objetivos
↓
Necesidades
↓
Información conocida
↓
Opciones
↓
Costes
↓
Riesgos
↓
Utilidad / reglas
↓
Decisión
↓
Resultado
```

Mostrar únicamente variables, reglas y estados explícitos del juego; no depender de cadenas de razonamiento privadas.

---

# 18. ECONOMÍA VIVA

Implementar:

- oferta;
- demanda;
- producción;
- consumo;
- almacenamiento;
- salarios;
- impuestos;
- crédito;
- deuda;
- seguros;
- inversión;
- comercio;
- logística;
- quiebra.

Las cadenas de suministro deben tener consecuencias reales. Una guerra puede romper rutas, subir precios, provocar escasez y generar consecuencias políticas.

---

# 19. GRANDES CASAS, TÍTULOS Y POLÍTICA

Implementar Grandes Casas con:

- familia gobernante;
- patrimonio;
- títulos;
- territorios;
- vasallos;
- flotas;
- economía;
- deuda;
- legitimidad;
- prestigio;
- influencia;
- alianzas;
- rivales;
- reclamaciones;
- facciones.

Jerarquía posible:

```text
Soberano
Gran Príncipe
Archiduque
Duque
Marqués
Conde
Vizconde
Barón
Señor / Dama
Caballero Juramentado
```

El **Alto Concilio de Casas** debe funcionar como una institución política propia y original con votos, favores, negociación, intereses y coaliciones.

---

# 20. PERSONAJES Y SUCESIÓN

Los personajes deben tener:

- edad;
- título;
- habilidades;
- personalidad;
- objetivos;
- lealtades;
- relaciones;
- patrimonio;
- reputación;
- deuda;
- ambición;
- historia.

La sucesión debe soportar:

- herencia;
- regencia;
- pretendientes;
- ramas familiares;
- disputas;
- reconocimiento;
- guerra civil;
- intervención exterior.

La muerte de un personaje importante debe producir consecuencias persistentes.

---

# 21. GALAXIA PROCEDURAL

Usar una semilla global reproducible.

Pipeline:

```text
Seed
↓
Galaxy Structure
↓
Regions
↓
Systems
↓
Stars
↓
Planets
↓
Moons
↓
Asteroids
↓
Resources
↓
Habitability
↓
Civilizations
↓
Organizations
↓
Trade Routes
↓
Initial Economy
↓
Initial History
```

La misma semilla debe reproducir el estado inicial del universo.

---

# 22. HISTORIA EMERGENTE

Registrar eventos importantes:

- guerras;
- sucesiones;
- quiebras;
- descubrimientos;
- fundaciones;
- colonizaciones;
- cambios de gobierno;
- tratados;
- desastres;
- crisis económicas.

Alimentar:

```text
Event Log
→ Timeline
→ Galactic Journal
→ Missions
→ Reputation
→ Diplomacy
→ UI Narratives
```

---

# 23. JUGADOR Y PROGRESIÓN

Permitir comenzar como:

- capitán;
- comerciante;
- explorador;
- minero;
- mercenario;
- industrial;
- oficial;
- noble;
- gobernador;
- almirante;
- líder de Casa;
- soberano.

No imponer una campaña lineal. Permitir ascenso social, cambio de profesión, fracaso, muerte, exilio, bancarrota y continuidad mediante herederos u otras entidades.

---

# 24. COLONIAS Y LOGÍSTICA

Soportar:

- puestos avanzados;
- estaciones;
- colonias;
- astilleros;
- laboratorios;
- instalaciones mineras;
- centros industriales.

La población, infraestructura, estabilidad, producción y logística deben evolucionar con el tiempo.

---

# 25. MODDING

Diseñar desde temprano una frontera de datos para poder modificar:

- naves;
- armas;
- módulos;
- recursos;
- bienes;
- organizaciones;
- Casas;
- títulos;
- tecnologías;
- eventos;
- misiones;
- leyes;
- reglas de generación.

Los sistemas centrales deben ser código; el contenido debe ser datos siempre que sea razonable.

---

# 26. HERRAMIENTAS INTERNAS

Crear progresivamente:

### Galaxy Inspector
Sistemas, recursos, organizaciones, rutas, fronteras.

### Entity Inspector
Estado completo de una entidad.

### AI Inspector
Decisiones y variables.

### Economy Inspector
Producción, inventario, precio, rutas y consumo.

### Simulation Profiler
CPU por sistema, jobs, memoria y tiempos de tick.

### Time Controller
Pausa y aceleración.

### Event Browser
Historial de acontecimientos.

### Save Inspector
Estado y versión de partidas.

Estas herramientas forman parte del proyecto, no son un lujo posterior.

---

# 27. TESTS Y DETERMINISMO

Crear tests para:

- generación procedural;
- determinismo;
- economía;
- producción;
- logística;
- IA;
- política;
- sucesión;
- combate;
- sensores;
- daño;
- save/load.

Cuando corresponda:

```text
misma semilla
+
misma configuración
+
mismo estado inicial
+
misma secuencia de acciones
=
misma salida
```

---

# 28. BENCHMARKS DESDE EL PRIMER MES

No asumir que la arquitectura escala. Medirla.

Crear escenarios de prueba progresivos:

```text
1 sistema
10 sistemas
100 sistemas
1.000 sistemas
5.000 sistemas
10.000 sistemas
```

Medir como mínimo:

- CPU;
- memoria;
- tiempo por tick;
- jobs;
- entidades activas;
- eventos;
- generación;
- guardado;
- carga.

El objetivo de la arquitectura es poder mantener un universo enorme sin calcular a máxima resolución todo, todo el tiempo.

---

# 29. REGLA DE PERFORMANCE

Antes de implementar una entidad o sistema altamente detallado, responder:

1. ¿Necesita existir físicamente?
2. ¿Puede agregarse?
3. ¿Puede ser un evento?
4. ¿Puede actualizarse menos frecuentemente?
5. ¿Puede materializarse solo cerca del jugador?
6. ¿Puede procesarse por lotes?
7. ¿Puede ejecutarse en paralelo?

Aplicar esta disciplina constantemente.

---

# 30. REGLA DE CAUSALIDAD

No crear una falsa sensación de simulación con números desconectados.

Si una guerra destruye una ruta, deben cambiar realmente:

- logística;
- precios;
- contratos;
- disponibilidad de bienes;
- política;
- decisiones de organizaciones.

Si muere un gobernante, deben cambiar realmente:

- sucesión;
- legitimidad;
- relaciones;
- facciones;
- política.

Las historias emergen de sistemas causales.

---

# 31. REGLA SOBRE EL JUGADOR

El jugador NO es el centro lógico de la galaxia.

No generar acontecimientos artificiales solamente para llamar su atención.

La galaxia debe poder:

- prosperar sin él;
- entrar en guerra sin él;
- formar nuevas organizaciones sin él;
- destruir Casas sin él;
- generar nuevos líderes sin él;
- crear rutas comerciales sin él;
- colonizar mundos sin él;
- cambiar sus fronteras sin él.

---

# 32. ROADMAP DE PRODUCCIÓN

## Fase 0 — Foundation

- repositorio;
- C++;
- build system;
- logging;
- assertions;
- tests;
- math;
- memory;
- jobs;
- profiling.

## Fase 1 — Simulation Kernel

- simulation clock;
- entity/state model;
- event system;
- deterministic update;
- headless simulation;
- save/load inicial.

## Fase 2 — Space Prototype

- sistema estelar;
- nave;
- movimiento;
- cámara;
- sensores;
- armas;
- combate;
- daño.

## Fase 3 — Economy Prototype

- recursos;
- estaciones;
- producción;
- mercado;
- comercio;
- logística.

## Fase 4 — Living World

- personajes;
- organizaciones;
- IA;
- misiones;
- acontecimientos;
- historia.

## Fase 5 — Political Simulation

- Casas;
- títulos;
- sucesión;
- vasallaje;
- Concilio;
- diplomacia.

## Fase 6 — Colonization

- puestos;
- colonias;
- población;
- infraestructura;
- producción planetaria.

## Fase 7 — Massive Simulation

Escalar con benchmarks desde 1 hasta miles de sistemas.

## Fase 8 — Advanced Destruction

- destrucción localizada;
- fragmentación;
- debris;
- estaciones destructibles;
- optimizaciones físicas.

## Fase 9 — UX + Content

- UI completa;
- mapas;
- administración;
- diario;
- herramientas de usuario;
- tutorial;
- contenido.

---

# 33. WORKFLOW OBLIGATORIO DE CLAUDE CODE

Para cualquier tarea:

1. Leer el contexto.
2. Inspeccionar el repositorio.
3. Buscar implementaciones existentes antes de crear nuevas.
4. Identificar dependencias.
5. Diseñar el cambio mínimo necesario.
6. Implementar.
7. Compilar.
8. Ejecutar tests.
9. Ejecutar benchmarks si afecta a sistemas de alto coste.
10. Revisar errores.
11. Corregir.
12. Documentar.
13. Mostrar qué cambió y qué falta.

No inventes APIs.

No dupliques sistemas.

No reescribas todo el proyecto sin justificarlo.

Si una tarea es demasiado grande, divídela en slices.

Si una decisión técnica es incierta, construye un experimento pequeño y mídelo antes de comprometer la arquitectura.

---

# 34. PRIMERA TAREA QUE DEBES EJECUTAR

NO crees todavía la galaxia completa.

Primero:

1. inspecciona el repositorio;
2. lee `game.md`;
3. lee `CLAUDE.md`;
4. analiza las referencias visuales;
5. identifica contradicciones, especialmente la antigua propuesta de Godot;
6. presenta la arquitectura del motor C++;
7. crea el build system;
8. crea el Core mínimo;
9. crea Simulation Clock;
10. crea Job System mínimo;
11. crea simulación headless;
12. crea logging/assertions/profiling;
13. crea el primer benchmark;
14. crea tests básicos;
15. compila y ejecuta todo.

Después DETENTE y presenta:

- estructura creada;
- decisiones arquitectónicas;
- resultados de compilación;
- resultados de tests;
- resultados del benchmark;
- riesgos detectados;
- siguiente milestone propuesto.

No implementes todavía el resto del juego.

---

# 35. CRITERIO FINAL

Construye primero el **motor capaz de soportar el juego** y después escala el contenido.

La simulación es el corazón.

El renderizado es una capa de presentación.

La interfaz muestra el estado real de la simulación.

El jugador es un actor dentro del universo, no el controlador omnisciente del universo.

La meta final es un sandbox galáctico persistente, emergente y altamente modificable en el que miles de sistemas, organizaciones, personajes, economías, flotas y conflictos puedan evolucionar durante años de tiempo simulado sin depender de una secuencia narrativa preescrita.
