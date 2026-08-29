# Original assignment diagram — Mermaid transcription

> This diagram is a semantic transcription of the supplied “big picture” page. It preserves the boxes and intended flows, but Mermaid cannot reproduce the exact line routing of the source image. This is source material, not the adopted HW4 implementation design.

```mermaid
flowchart TB
    DATA["Data Sources<br/>(Databento JSON / Feather files)"]

    subgraph PY["Python Layer"]
        VIS["Visualization & Analysis<br/>(matplotlib, Plotly, pandas, Streamlit)"]
        PARAM["Strategy Parameter Setup<br/>(Python script / Jupyter Notebook)"]
    end

    API["API"]

    subgraph CPP["C++ Process"]
        subgraph BT["Backtest Engine Thread"]
            MERGER["Event Merger<br/>(Flat / Hierarchy)"]
            DISPATCH["Chronological Dispatcher"]
            LOBS["Map of LOBs<br/>(per instrument)"]
            PUBLISHER["Market Data Publisher"]
            SLIPPAGE["Slippage Simulator"]
            GW_SERVER["Order Gateway Server"]

            MERGER --> DISPATCH
            DISPATCH --> LOBS
            SLIPPAGE --> LOBS
            GW_SERVER --> LOBS
            LOBS --> PUBLISHER
        end

        subgraph TE["Trading Engine Thread"]
            CONSUMER["Market Data Consumer"]
            SIM["LOB Simulation,<br/>Order Matching & Fills"]
            FEATURES["Feature Generator"]
            STRATEGY["Strategy Logic"]
            ORDERS["Order Manager /<br/>Position Keeper"]
            GW_CLIENT["Order Gateway Client"]
            RISK["Risk Engine"]

            CONSUMER --> SIM
            SIM --> FEATURES
            FEATURES --> STRATEGY
            SIM --> ORDERS
            ORDERS --> GW_CLIENT
            ORDERS --> RISK
            SIM --> RISK
        end

        PUBLISHER --> CONSUMER
        GW_CLIENT --> GW_SERVER
    end

    DATA --> MERGER
    PARAM --> API --> STRATEGY
```

## Ambiguities visible in the source diagram

- It contains both trading-side “LOB Simulation, Order Matching & Fills” and backtest-side Gateway/Slippage components, so the authoritative fill producer is not explicit.
- The exact API return path to Python is not drawn.
- The diagram places “Strategy Logic” inside the C++ process, while Homework 4 requires a Python-defined strategy through pybind11.
- The Slippage Simulator and Order Gateway Server appear to modify the map of LOBs outside the Chronological Dispatcher, leaving same-timestamp ordering unspecified.

The adopted project resolution of these ambiguities is documented under [`../architecture`](../architecture/).
