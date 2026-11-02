//! Replays a captured JSON-RPC compile+render pair and times each stage.
//! Usage: cargo run --release --example replay -- requests.log
use liquid::{convert, json, MemoryFileSystem, RenderOptions, Template};
use std::time::Instant;

fn main() {
    let path = std::env::args().nth(1).expect("path");
    let text = std::fs::read_to_string(path).unwrap();
    let mut tpl = None;
    let mut fs = MemoryFileSystem::new();
    for line in text.lines() {
        let req = json::parse(line).unwrap();
        let params = req.get("params").cloned().unwrap_or(json::Json::Null);
        match req.get("method").and_then(|m| m.as_str()) {
            Some("compile") => {
                let src = params.get("template").and_then(|s| s.as_str()).unwrap().to_string();
                let t0 = Instant::now();
                tpl = Some(Template::parse(&src).unwrap());
                println!("parse template: {:?}", t0.elapsed());
                if let Some(json::Json::Object(m)) = params.get("filesystem") {
                    for (k, v) in m {
                        fs.insert(k, v.as_str().unwrap_or("").to_string());
                    }
                }
            }
            Some("render") => {
                let t = tpl.as_ref().unwrap();
                for _ in 0..3 {
                    let t0 = Instant::now();
                    let j = json::parse(line).unwrap();
                    let t1 = Instant::now();
                    let env = j.get("params").unwrap().get("environment").unwrap();
                    let assigns = convert::json_to_assigns(env, &mut |_| None).unwrap();
                    let t2 = Instant::now();
                    let opts = RenderOptions { strict_errors: false, file_system: Some(&fs), ..Default::default() };
                    let r = t.render(assigns, &opts).unwrap();
                    let t3 = Instant::now();
                    println!("json {:?} convert {:?} render {:?} out={}B errors={}", t1 - t0, t2 - t1, t3 - t2, r.output.len(), r.errors.len());
                }
                let j = json::parse(line).unwrap();
                let env = j.get("params").unwrap().get("environment").unwrap();
                let assigns = std::rc::Rc::new(convert::json_to_assigns(env, &mut |_| None).unwrap());
                let opts = RenderOptions { strict_errors: false, file_system: Some(&fs), ..Default::default() };
                let n = 500;
                let t0 = Instant::now();
                for _ in 0..n {
                    t.render_shared(assigns.clone(), &opts).unwrap();
                }
                println!("resident render avg {:?}", t0.elapsed() / n);
            }
            _ => {}
        }
    }
}
