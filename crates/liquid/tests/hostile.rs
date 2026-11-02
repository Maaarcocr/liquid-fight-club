//! Hostile templates and data: every case must terminate quickly, without
//! crashing, on a thread with a small stack.

use liquid::{Hash, MemoryFileSystem, RenderOptions, Template, Value};
use std::time::{Duration, Instant};

const STACK: usize = 512 * 1024;

fn run_with(src: String, partials: Vec<(&'static str, String)>, assigns_json: &'static str) -> (String, Duration) {
    let h = std::thread::Builder::new()
        .stack_size(STACK)
        .spawn(move || {
            let start = Instant::now();
            let out = match Template::parse(&src) {
                Err(e) => e.to_string(),
                Ok(t) => {
                    let mut fs = MemoryFileSystem::new();
                    for (k, v) in partials {
                        fs.insert(k, v);
                    }
                    let j = liquid::json::parse(assigns_json).unwrap();
                    let assigns = liquid::convert::json_to_assigns(&j, &mut |_| None).unwrap();
                    let opts = RenderOptions { strict_errors: false, file_system: Some(&fs), ..RenderOptions::default() };
                    match t.render(assigns, &opts) {
                        Ok(r) => r.output,
                        Err(e) => e.to_string(),
                    }
                }
            };
            (out, start.elapsed())
        })
        .unwrap();
    h.join().expect("render thread crashed")
}

fn run(src: &str) -> (String, Duration) {
    run_with(src.to_string(), Vec::new(), "{}")
}

fn assert_fast(name: &str, d: Duration) {
    assert!(d < Duration::from_secs(10), "{} took {:?}", name, d);
}

#[test]
fn deep_block_nesting_is_rejected() {
    let src = format!("{}x{}", "{% if true %}".repeat(5000), "{% endif %}".repeat(5000));
    let (out, d) = run(&src);
    assert!(out.contains("Nesting too deep"), "{}", &out[..out.len().min(200)]);
    assert_fast("nesting", d);
}

#[test]
fn deep_liquid_tag_nesting() {
    let src = format!("{{% liquid {}echo 1 %}}", "liquid ".repeat(5000));
    let (_, d) = run(&src);
    assert_fast("liquid nesting", d);
}

#[test]
fn recursive_include_and_render() {
    for tag in ["include", "render"] {
        let body = format!("{}{{% {} 'self' %}}{}", "{% if true %}".repeat(90), tag, "{% endif %}".repeat(90));
        let (out, d) = run_with(format!("{{% {} 'self' %}}", tag), vec![("self", body)], "{}");
        assert!(out.contains("Nesting too deep"), "{}: {}", tag, &out[..out.len().min(300)]);
        assert_fast(tag, d);
    }
}

#[test]
fn huge_loops_are_bounded() {
    for src in [
        "{% for i in (1..10000000000) %}x{% endfor %}",
        "{% for a in (1..100000) %}{% for b in (1..100000) %}{% endfor %}{% endfor %}",
        "{% tablerow i in (1..10000000000) %}{% endtablerow %}",
        "{{ (1..100000000000) | join }}",
    ] {
        let (out, d) = run(src);
        assert!(out.contains("Liquid error"), "{}: {}", src, &out[..out.len().min(200)]);
        assert_fast(src, d);
    }
}

#[test]
fn string_growth_is_bounded() {
    for src in [
        "{% assign s = 'x' %}{% for i in (1..100) %}{% assign s = s | append: s %}{% endfor %}{{ s | size }}",
        "{% for i in (1..10000000) %}{% assign s = s | append: '0123456789abcdef' %}{% endfor %}",
        "{% capture s %}x{% endcapture %}{% for i in (1..60) %}{% capture s %}{{ s }}{{ s }}{% endcapture %}{% endfor %}",
        "{% assign s = 'ab' %}{% for i in (1..60) %}{% assign s = s | replace: 'a', s %}{% endfor %}",
        "{% assign s = 'abcdefghij' %}{% for i in (1..40) %}{% assign s = s | split: '' | join: s %}{% endfor %}",
    ] {
        let (out, d) = run(src);
        assert!(out.len() < 300 * 1024 * 1024, "{}", src);
        assert_fast(src, d);
    }
}

#[test]
fn bigint_growth_is_bounded() {
    let (out, d) = run("{% assign x = 3 %}{% for i in (1..1000) %}{% assign x = x | times: x %}{% endfor %}{{ x | size }}");
    assert_fast("bigint", d);
    assert!(!out.is_empty());
    let (_, d) = run(
        "{% assign x = 3 %}{% for i in (1..14) %}{% assign x = x | times: x %}{% endfor %}{% for i in (1..1000000) %}{% assign y = x | times: x | divided_by: 7 | modulo: 13 %}{% endfor %}",
    );
    assert_fast("bigint loop", d);
}

#[test]
fn pathological_tokens() {
    for src in [
        "{%".repeat(200_000),
        "{{ {%".repeat(100_000),
        "{{".repeat(200_000),
        format!("{{% assign {} %}}", "a".repeat(1_000_000)),
        format!("{{{{ {} }}}}", "a.".repeat(200_000)),
        format!("{{{{ {} }}}}", "[".repeat(100_000)),
        format!("{{{{ x{} }}}}", "[0]".repeat(100_000)),
        format!("{{% if {} %}}{{% endif %}}", vec!["a"; 100_000].join(" and ")),
        format!("{{{{ x {} }}}}", "| upcase ".repeat(100_000)),
        format!("{{% cycle {} %}}", vec!["'a'"; 100_000].join(", ")),
        format!("{{% comment %}}{}{{% endcomment %}}", "{% comment %}".repeat(100_000)),
        format!("{}", "{% raw %}".repeat(100_000)),
        format!("{{% liquid {} %}}", "\n".repeat(1_000_000)),
    ] {
        let (_, d) = run(&src);
        assert_fast(&src[..20.min(src.len())], d);
    }
}

#[test]
fn deep_data() {
    let mut j = String::new();
    for _ in 0..250 {
        j.push_str("{\"a\":[");
    }
    for _ in 0..250 {
        j.push_str("]}");
    }
    let doc = format!("{{\"x\":{}}}", j);
    let leaked: &'static str = Box::leak(doc.into_boxed_str());
    for src in [
        "{{ x }}",
        "{{ x | json }}",
        "{{ x | append: '' }}",
        "{% if x == x %}eq{% endif %}",
        "{{ x | sort | size }}",
        "{% for i in x %}{{ i }}{% endfor %}",
        "{{ x | flatten | size }}",
        "{{ x.a | uniq | size }}",
    ] {
        let (_, d) = run_with(src.to_string(), Vec::new(), leaked);
        assert_fast(src, d);
    }
}

#[test]
fn many_partials() {
    let mut parts = Vec::new();
    let names: Vec<&'static str> = (0..50).map(|i| &*Box::leak(format!("p{}", i).into_boxed_str())).collect();
    for (i, n) in names.iter().enumerate() {
        let next = if i + 1 < names.len() { format!("{{% include '{}' %}}{{% include '{}' %}}", names[i + 1], names[i + 1]) } else { "x".into() };
        parts.push((*n, next));
    }
    let (_, d) = run_with("{% include 'p0' %}".to_string(), parts, "{}");
    assert_fast("exponential includes", d);
}

#[test]
fn render_twice_is_identical() {
    let t = Template::parse("{% cycle 'a','b' %}{% increment x %}{% for i in (1..3) %}{{ i }}{% endfor %}").unwrap();
    let opts = RenderOptions::default();
    let a = t.render(Hash::new(), &opts).unwrap().output;
    let b = t.render(Hash::new(), &opts).unwrap().output;
    assert_eq!(a, b);
    let _ = Value::Nil;
}
