package FindJavaTest;
import java.lang.reflect.Method;
public class ReflectJavaMain {
    public static void main(String[] args) {
        
        Class<JavaMain> javaMainClass = JavaMain.class;

        Method[] methods = javaMainClass.getDeclaredMethods();
        var className = javaMainClass.getName();
        System.out.println("Class Name: " + className);
        for (Method method : methods) {
            System.out.println(method);
        }

    }
}
